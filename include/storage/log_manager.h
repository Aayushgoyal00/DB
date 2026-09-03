#pragma once

#include <fstream>
#include <mutex>
#include <string>

#include "common/config.h"
#include "common/status.h"
#include "storage/log_record.h"

namespace dbengine {

// On-disk layout of a single framed log record:
//   u32  body_len
//   char body[body_len]            ← LogRecord::Encode() output
//   u32  crc32 (over body_len + body)
//
// CRC lets us detect torn writes at the file tail. LSN is assigned by
// LogManager and stored only in memory (not in the record body) so that
// recovery doesn't have to rewrite records to renumber them.

class LogManager {
 public:
  // Opens (or creates) the log file. If the file is non-empty on open,
  // recovery is the caller's responsibility — LogManager only appends.
  explicit LogManager(const std::string& log_file_path);
  ~LogManager();

  LogManager(const LogManager&) = delete;
  LogManager& operator=(const LogManager&) = delete;

  // Append a record to the in-memory buffer. Assigns r.lsn. Does NOT
  // force the log to disk — that's Flush()'s job, controlled by the
  // caller (typically at COMMIT time).
  Status Append(LogRecord* r);

  // Force the entire in-memory buffer to disk (fsync). After this returns,
  // GetFlushedLSN() reflects everything that was Append()'d before the call.
  Status Flush();

  // Convenience: Append() then Flush() in one call. Used at COMMIT.
  Status AppendAndFlush(LogRecord* r);

  // Highest LSN that is known-durable on disk. The buffer pool asks this
  // before flushing a dirty page to enforce write-ahead.
  lsn_t GetFlushedLSN() const { return flushed_lsn_; }

  // Highest LSN ever assigned (may be > flushed_lsn_).
  lsn_t GetNextLSN() const { return next_lsn_; }

  // Reads every record from offset 0 forward, calling visit(record) for
  // each successfully decoded record. Stops on the first decode failure
  // (torn write) — that position is the recovery start point. Used by
  // RecoveryManager during analysis.
  template <typename Visitor>
  Status IterateAll(Visitor visit);

 private:
  Status WriteRaw(const char* data, size_t len);

  std::string log_file_path_;
  std::fstream log_io_;
  std::mutex latch_;
  std::string buffer_;
  lsn_t next_lsn_ = 1;     // LSNs start at 1; 0 is reserved for INVALID_LSN
  lsn_t flushed_lsn_ = 0;
};

template <typename Visitor>
Status LogManager::IterateAll(Visitor visit) {
  std::lock_guard<std::mutex> lock(latch_);
  std::ifstream in(log_file_path_, std::ios::binary);
  if (!in) return Status::IOError("cannot open log for iteration: " + log_file_path_);

  lsn_t cur_lsn = 1;
  while (true) {
    uint32_t body_len = 0;
    if (!in.read(reinterpret_cast<char*>(&body_len), 4)) break;  // EOF
    std::string body(body_len, '\0');
    if (body_len > 0 && !in.read(body.data(), body_len)) break;
    uint32_t crc_stored = 0;
    if (!in.read(reinterpret_cast<char*>(&crc_stored), 4)) break;

    uint32_t crc_computed = 0;
    {
      const char* p = reinterpret_cast<const char*>(&body_len);
      for (size_t i = 0; i < 4; ++i) crc_computed = crc_computed * 31u + static_cast<uint8_t>(p[i]);
      for (size_t i = 0; i < body.size(); ++i)
        crc_computed = crc_computed * 31u + static_cast<uint8_t>(body[i]);
    }
    if (crc_computed != crc_stored) {
      return Status::Corruption("torn log write at offset");
    }

    LogRecord rec;
    if (!LogRecord::Decode(body.data(), body.size(), &rec)) {
      return Status::Corruption("malformed log record");
    }
    rec.lsn = cur_lsn++;
    visit(rec);
  }
  return Status::OK();
}

}  // namespace dbengine
