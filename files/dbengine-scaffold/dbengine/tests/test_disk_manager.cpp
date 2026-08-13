#include <cstdio>
#include <cstring>
#include <iostream>

#include "common/config.h"
#include "storage/disk_manager.h"

using dbengine::DiskManager;
using dbengine::PAGE_SIZE;
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

} // namespace

int main() {
  const char* kTestFile = "test_disk_manager.db";
  std::remove(kTestFile); // start from a clean file every run

  {
    DiskManager dm(kTestFile);
    Check(dm.GetNumPages() == 0, "fresh file starts with 0 pages");

    dbengine::page_id_t pid = dm.AllocatePage();
    Check(pid == 0, "first allocated page id is 0");
    Check(dm.GetNumPages() == 1, "num_pages incremented after allocate");

    char write_buf[PAGE_SIZE];
    std::memset(write_buf, 0, PAGE_SIZE);
    std::strcpy(write_buf, "hello page zero");

    Status write_status = dm.WritePage(pid, write_buf);
    Check(write_status.ok(), "WritePage succeeds for a valid page id");

    char read_buf[PAGE_SIZE];
    std::memset(read_buf, 0, PAGE_SIZE);
    Status read_status = dm.ReadPage(pid, read_buf);
    Check(read_status.ok(), "ReadPage succeeds for a written page");
    Check(std::strcmp(read_buf, "hello page zero") == 0,
          "read bytes match written bytes");

    Status bad_read = dm.ReadPage(99, read_buf);
    Check(!bad_read.ok() && bad_read.IsIOError() == false,
          "reading an unallocated page id returns an error status");
  }

  {
    // Reopen the same file and confirm allocation state survived restart.
    DiskManager dm(kTestFile);
    Check(dm.GetNumPages() == 1,
          "num_pages recovered correctly after reopening the file");

    char read_buf[PAGE_SIZE];
    Status read_status = dm.ReadPage(0, read_buf);
    Check(read_status.ok() && std::strcmp(read_buf, "hello page zero") == 0,
          "data written before restart is still readable after restart");
  }

  std::remove(kTestFile);

  if (g_failures == 0) {
    std::cout << "\nAll checks passed.\n";
    return 0;
  }
  std::cout << "\n" << g_failures << " check(s) failed.\n";
  return 1;
}
