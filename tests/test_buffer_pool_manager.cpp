#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "storage/buffer_pool_manager.h"
#include "storage/disk_manager.h"
#include "storage/log_manager.h"

using dbengine::BufferPoolManager;
using dbengine::DiskManager;
using dbengine::LogManager;
using dbengine::LogRecord;
using dbengine::MakeInsertRecord;
using dbengine::Page;
using dbengine::page_id_t;
using dbengine::Status;

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& what) {
  if (!condition) {
    std::cerr << "FAIL: " << what << "\n";
    ++g_failures;
  } else {
    std::cout << "PASS: " << what << "\n";
  }
}

void TestBasicBufferPool() {
  const char* db_name = "test_bpm_basic.db";
  std::remove(db_name);
  {
    DiskManager disk_manager(db_name);
    BufferPoolManager bpm(5, &disk_manager);

    page_id_t pid0 = -1;
    Page* p0 = bpm.NewPage(&pid0);
    Check(p0 != nullptr && pid0 == 0, "allocate new page 0");
    Check(p0->PinCount() == 1, "new page has pin count 1");
    std::strcpy(p0->GetData(), "Hello Page 0");

    page_id_t pid1 = -1;
    Page* p1 = bpm.NewPage(&pid1);
    Check(p1 != nullptr && pid1 == 1, "allocate new page 1");
    std::strcpy(p1->GetData(), "Hello Page 1");

    Check(bpm.UnpinPage(pid0, true), "unpin page 0 as dirty");
    Check(bpm.UnpinPage(pid1, true), "unpin page 1 as dirty");

    Page* fetched0 = bpm.FetchPage(0);
    Check(fetched0 != nullptr, "fetch page 0 from pool");
    Check(std::strcmp(fetched0->GetData(), "Hello Page 0") == 0, "fetched page 0 content matches");
    Check(bpm.UnpinPage(0, false), "unpin page 0");

    Check(bpm.FlushAllPages().ok(), "flush all pages to disk");
  }

  // Reopen and verify persisted data on disk
  {
    DiskManager disk_manager(db_name);
    BufferPoolManager bpm(5, &disk_manager);

    Page* fetched0 = bpm.FetchPage(0);
    Check(fetched0 != nullptr, "fetch page 0 after reopen");
    Check(std::strcmp(fetched0->GetData(), "Hello Page 0") == 0, "reopened page 0 content matches disk");
    Check(bpm.UnpinPage(0, false), "unpin page 0");

    Page* fetched1 = bpm.FetchPage(1);
    Check(fetched1 != nullptr, "fetch page 1 after reopen");
    Check(std::strcmp(fetched1->GetData(), "Hello Page 1") == 0, "reopened page 1 content matches disk");
    Check(bpm.UnpinPage(1, false), "unpin page 1");
  }
  std::remove(db_name);
}

void TestEvictionUnderPressure() {
  const char* db_name = "test_bpm_eviction.db";
  std::remove(db_name);
  {
    DiskManager disk_manager(db_name);
    // Small buffer pool of 3 frames
    BufferPoolManager bpm(3, &disk_manager);

    page_id_t pids[5];
    for (int i = 0; i < 3; ++i) {
      Page* p = bpm.NewPage(&pids[i]);
      Check(p != nullptr, "allocate page in small pool");
      std::string content = "data-" + std::to_string(i);
      std::strcpy(p->GetData(), content.c_str());
    }

    // Try allocating 4th page while all 3 are pinned -> should fail (return nullptr)
    page_id_t overflow_pid = -1;
    Page* overflow_page = bpm.NewPage(&overflow_pid);
    Check(overflow_page == nullptr, "cannot allocate new page when all frames are pinned");

    // Unpin page 0 (make it evictable)
    Check(bpm.UnpinPage(pids[0], true), "unpin page 0");

    // Now allocating 4th page should evict page 0
    Page* p3 = bpm.NewPage(&pids[3]);
    Check(p3 != nullptr, "allocate 4th page after evicting page 0");
    std::strcpy(p3->GetData(), "data-3");
    Check(bpm.UnpinPage(pids[3], true), "unpin 4th page");

    // Unpin page 1 and page 2
    Check(bpm.UnpinPage(pids[1], true), "unpin page 1");
    Check(bpm.UnpinPage(pids[2], true), "unpin page 2");

    // Fetch page 0 again -> triggers load from disk, evicting another unpinned frame
    Page* reload0 = bpm.FetchPage(pids[0]);
    Check(reload0 != nullptr, "reload evicted page 0 from disk");
    Check(std::strcmp(reload0->GetData(), "data-0") == 0, "reloaded page 0 has correct content written on eviction");
    Check(bpm.UnpinPage(pids[0], false), "unpin reloaded page 0");
  }
  std::remove(db_name);
}

void TestWriteAheadLoggingHook() {
  const char* db_name = "test_bpm_wal.db";
  const char* log_name = "test_bpm_wal.log";
  std::remove(db_name);
  std::remove(log_name);
  {
    DiskManager disk_manager(db_name);
    LogManager log_manager(log_name);
    BufferPoolManager bpm(2, &disk_manager, &log_manager);

    page_id_t pid0 = -1;
    Page* p0 = bpm.NewPage(&pid0);
    Check(p0 != nullptr, "allocate page 0 with WAL");
    std::strcpy(p0->GetData(), "WAL Page 0");

    // Append log record without flushing
    LogRecord rec = MakeInsertRecord(1, pid0, "key0", "WAL Page 0");
    Check(log_manager.Append(&rec).ok(), "append log record (unflushed)");
    p0->SetPageLSN(rec.lsn);

    Check(log_manager.GetFlushedLSN() < rec.lsn, "WAL is not yet flushed to disk");

    // Unpin dirty page
    bpm.UnpinPage(pid0, true);

    // Evict pages until page 0 is evicted
    page_id_t pid1, pid2, pid3;
    bpm.NewPage(&pid1);
    bpm.UnpinPage(pid1, false);
    bpm.NewPage(&pid2);
    bpm.UnpinPage(pid2, false);
    bpm.NewPage(&pid3);
    bpm.UnpinPage(pid3, false);

    // Eviction must have triggered log_manager.Flush() before writing dirty page 0 to disk!
    Check(log_manager.GetFlushedLSN() >= rec.lsn, "WAL was flushed ahead of dirty page eviction");
  }
  std::remove(db_name);
  std::remove(log_name);
}

}  // namespace

int main() {
  TestBasicBufferPool();
  TestEvictionUnderPressure();
  TestWriteAheadLoggingHook();

  if (g_failures == 0) {
    std::cout << "\nAll BufferPoolManager checks passed.\n";
    return 0;
  }
  std::cout << "\n" << g_failures << " check(s) failed.\n";
  return 1;
}
