#include "storage/log_manager.h"

#include <cstring>

namespace dbengine {

LogManager::LogManager(const std::string& log_file_path)
    : log_file_path_(log_file_path) {
  log_io_.open(log_file_path_,
               std::ios::binary | std::ios::in | std::ios::out | std::ios::app);
  if (!log_io_.is_open()) {
    // File didn't exist — try creating it.
    log_io_.open(log_file_path_, std::ios::binary | std::ios::out | std::ios::trunc);
    log_io_.close();
    log_io_.open(log_file_path_,
                 std::ios::binary | std::ios::in | std::ios::out);
  }
}

LogManager::~LogManager() {
  std::lock_guard<std::mutex> lock(latch_);
  if (!buffer_.empty()) {
    WriteRaw(buffer_.data(), buffer_.size());
    buffer_.clear();
  }
  log_io_.flush();
  log_io_.close();
}

Status LogManager::Append(LogRecord* r) {
  std::lock_guard<std::mutex> lock(latch_);
  r->lsn = next_lsn_++;

  std::string body = r->Encode();
  uint32_t body_len = static_cast<uint32_t>(body.size());
  uint32_t crc = 0;
  const char* p = reinterpret_cast<const char*>(&body_len);
  for (size_t i = 0; i < 4; ++i) crc = crc * 31u + static_cast<uint8_t>(p[i]);
  for (size_t i = 0; i < body.size(); ++i)
    crc = crc * 31u + static_cast<uint8_t>(body[i]);

  buffer_.append(reinterpret_cast<const char*>(&body_len), 4);
  buffer_.append(body);
  buffer_.append(reinterpret_cast<const char*>(&crc), 4);
  return Status::OK();
}

Status LogManager::WriteRaw(const char* data, size_t len) {
  if (!log_io_.write(data, static_cast<std::streamsize>(len))) {
    return Status::IOError("log write failed");
  }
  if (!log_io_.flush()) {
    return Status::IOError("log flush failed");
  }
  return Status::OK();
}

Status LogManager::Flush() {
  std::lock_guard<std::mutex> lock(latch_);
  if (buffer_.empty()) return Status::OK();
  Status s = WriteRaw(buffer_.data(), buffer_.size());
  if (!s.ok()) return s;
  buffer_.clear();
  flushed_lsn_ = next_lsn_ - 1;
  return Status::OK();
}

Status LogManager::AppendAndFlush(LogRecord* r) {
  Status s = Append(r);
  if (!s.ok()) return s;
  return Flush();
}

}  // namespace dbengine
