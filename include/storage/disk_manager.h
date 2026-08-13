#pragma once

#include <fstream>
#include <mutex>
#include <string>

#include "common/config.h"
#include "common/status.h"

namespace dbengine {

// DiskManager owns the single on-disk file that backs the engine and is the
// ONLY class in the whole codebase allowed to call read/write/seek on it.
// Every other module (buffer pool, B+Tree, LSM engine) talks to disk
// exclusively through this interface. That boundary is what makes it
// possible to later swap in O_DIRECT, io_uring, or memory-mapped I/O without
// touching a single line of tree logic.
//
// Phase 0 scope: sequential page read/write + naive bump-pointer allocation.
// Phase 2 will add a free-list so deleted pages get reused instead of
// leaking disk space forever.
class DiskManager {
 public:
  explicit DiskManager(const std::string& db_file_path);
  ~DiskManager();

  DiskManager(const DiskManager&) = delete;
  DiskManager& operator=(const DiskManager&) = delete;

  // Reads PAGE_SIZE bytes at the offset for page_id into page_data.
  // page_data must point to a buffer of at least PAGE_SIZE bytes.
  Status ReadPage(page_id_t page_id, char* page_data);

  // Writes PAGE_SIZE bytes from page_data to the offset for page_id.
  Status WritePage(page_id_t page_id, const char* page_data);

  // Bump-pointer allocation: hands out the next unused page id and grows
  // the file. Freed pages are not reused until Phase 2 adds a free-list.
  page_id_t AllocatePage();

  // Total number of pages currently allocated in the file.
  size_t GetNumPages() const { return num_pages_; }

 private:
  std::fstream db_io_;
  std::string db_file_path_;
  size_t num_pages_ = 0;
  std::mutex io_mutex_; // guards db_io_; every method above takes this
};

} // namespace dbengine
