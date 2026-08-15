#include "storage/disk_manager.h"

#include <algorithm>
#include <array>

#include "storage/slotted_page.h"

namespace dbengine {

DiskManager::DiskManager(const std::string& db_file_path)
    : db_file_path_(db_file_path) {
  // Open for read/write, create if missing, without truncating existing
  // data — a second run against the same file should see prior pages.
  db_io_.open(db_file_path_,
              std::ios::binary | std::ios::in | std::ios::out);
  if (!db_io_.is_open()) {
    // File probably doesn't exist yet — create it, then reopen normally.
    db_io_.clear();
    db_io_.open(db_file_path_,
                std::ios::binary | std::ios::out | std::ios::trunc);
    db_io_.close();
    db_io_.open(db_file_path_,
                std::ios::binary | std::ios::in | std::ios::out);
  }

  // Recover num_pages_ from existing file size so a restart doesn't forget
  // what's already allocated.
  db_io_.seekg(0, std::ios::end);
  std::streampos file_size = db_io_.tellg();
  if (file_size > 0) {
    num_pages_ = static_cast<size_t>(file_size) / PAGE_SIZE;
  }

  // A free page has a durable marker in its first byte. This deliberately
  // trades startup scan time for a tiny implementation with no separate
  // allocation-map page; it is sufficient until a bitmap/free-list page is
  // warranted by larger databases.
  std::array<char, 1> page_type{};
  for (size_t i = 0; i < num_pages_; ++i) {
    db_io_.seekg(static_cast<std::streamoff>(i) * PAGE_SIZE);
    db_io_.read(page_type.data(), page_type.size());
    if (db_io_.good() &&
        static_cast<uint8_t>(page_type[0]) == static_cast<uint8_t>(PageType::kFree)) {
      free_pages_.push_back(static_cast<page_id_t>(i));
    }
    db_io_.clear();
  }
}

DiskManager::~DiskManager() {
  if (db_io_.is_open()) {
    db_io_.close();
  }
}

Status DiskManager::ReadPage(page_id_t page_id, char* page_data) {
  std::lock_guard<std::mutex> lock(io_mutex_);

  if (page_id < 0 || static_cast<size_t>(page_id) >= num_pages_) {
    return Status::InvalidArgument("ReadPage: page_id out of range");
  }

  const std::streamoff offset =
      static_cast<std::streamoff>(page_id) * PAGE_SIZE;
  db_io_.seekg(offset);
  db_io_.read(page_data, PAGE_SIZE);

  if (db_io_.bad()) {
    return Status::IOError("ReadPage: stream in bad state");
  }
  // gcount() < PAGE_SIZE would mean a short read (truncated/corrupt file).
  // Phase 1 will replace this with a checksum check instead of a length
  // check once pages carry a header with a stored checksum.
  if (static_cast<size_t>(db_io_.gcount()) < PAGE_SIZE) {
    db_io_.clear();
    return Status::Corruption("ReadPage: short read, file may be truncated");
  }

  db_io_.clear();
  return Status::OK();
}

Status DiskManager::WritePage(page_id_t page_id, const char* page_data) {
  std::lock_guard<std::mutex> lock(io_mutex_);

  if (page_id < 0 || static_cast<size_t>(page_id) >= num_pages_) {
    return Status::InvalidArgument("WritePage: page_id out of range");
  }

  const std::streamoff offset =
      static_cast<std::streamoff>(page_id) * PAGE_SIZE;
  db_io_.seekp(offset);
  db_io_.write(page_data, PAGE_SIZE);

  if (db_io_.bad()) {
    return Status::IOError("WritePage: stream in bad state");
  }

  // seekg/seekp in the next operation provides the required direction change
  // for this read/write stream. We deliberately do not flush each page: that
  // would turn every logical B+Tree update into a synchronous I/O barrier.
  // Phase 3's WAL supplies durable ordering; close() flushes clean shutdowns.
  return Status::OK();
}

page_id_t DiskManager::AllocatePage() {
  std::lock_guard<std::mutex> lock(io_mutex_);
  if (!free_pages_.empty()) {
    const page_id_t reused_page_id = free_pages_.back();
    free_pages_.pop_back();
    return reused_page_id;
  }
  page_id_t new_page_id = static_cast<page_id_t>(num_pages_);
  ++num_pages_;
  return new_page_id;
}

Status DiskManager::DeallocatePage(page_id_t page_id) {
  std::lock_guard<std::mutex> lock(io_mutex_);
  if (page_id < 0 || static_cast<size_t>(page_id) >= num_pages_) {
    return Status::InvalidArgument("DeallocatePage: page_id out of range");
  }
  if (std::find(free_pages_.begin(), free_pages_.end(), page_id) != free_pages_.end()) {
    return Status::InvalidArgument("DeallocatePage: page is already free");
  }
  std::array<char, PAGE_SIZE> free_page{};
  free_page[0] = static_cast<char>(PageType::kFree);
  const std::streamoff offset = static_cast<std::streamoff>(page_id) * PAGE_SIZE;
  db_io_.seekp(offset);
  db_io_.write(free_page.data(), PAGE_SIZE);
  if (db_io_.bad()) {
    return Status::IOError("DeallocatePage: stream in bad state");
  }
  free_pages_.push_back(page_id);
  return Status::OK();
}

} // namespace dbengine
