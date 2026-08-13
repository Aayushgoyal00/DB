#include "storage/disk_manager.h"

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

  // flush() forces the write out of libstdc++'s buffer. It does NOT force
  // an fsync — Phase 3's WAL work is what gives us real durability
  // guarantees. Flushing here just keeps ReadPage-after-WritePage
  // consistent within Phase 0/1/2 before the WAL exists.
  db_io_.flush();
  return Status::OK();
}

page_id_t DiskManager::AllocatePage() {
  std::lock_guard<std::mutex> lock(io_mutex_);
  page_id_t new_page_id = static_cast<page_id_t>(num_pages_);
  ++num_pages_;
  return new_page_id;
}

} // namespace dbengine
