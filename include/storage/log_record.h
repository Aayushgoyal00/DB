#pragma once

#include <cstdint>
#include <string>

#include "common/config.h"

namespace dbengine {

// Type of a single WAL record. The values matter because Recovery scans
// the log and dispatches on type — adding a new variant requires updating
// both the encoder and the recovery scanner.
enum class LogRecordType : uint8_t {
  kBegin = 0,
  kCommit = 1,
  kAbort = 2,
  kInsert = 3,    // before-image: empty; after-image: key + value
  kUpdate = 4,    // before-image: old value; after-image: new value
  kDelete = 5,    // before-image: old value; after-image: empty
  kCheckpoint = 6,
};

// One WAL record. Serialized to a self-describing byte string by Encode().
// On disk, an additional 4-byte length prefix and 4-byte CRC trailer wrap
// the encoded body so torn writes are detectable.
struct LogRecord {
  lsn_t lsn = INVALID_LSN;
  txn_id_t txn_id = INVALID_TXN_ID;
  LogRecordType type = LogRecordType::kBegin;
  page_id_t page_id = INVALID_PAGE_ID;

  // Key + before/after values. Empty string is valid (e.g. Insert has no
  // before-image). Together they identify the record's payload exactly.
  std::string key;
  std::string before_image;
  std::string after_image;

  // Serialize the record (without LSN — LSN is assigned by LogManager at
  // append time, not by the caller). Returns a byte string suitable for
  // length-prefixing and writing to the log file.
  std::string Encode() const;

  // Inverse of Encode. Returns false on malformed input (truncated, bad
  // type, oversized length).
  static bool Decode(const char* data, size_t len, LogRecord* out);
};

// Helper constructors — keeps call sites readable.
LogRecord MakeBeginRecord(txn_id_t txn_id);
LogRecord MakeCommitRecord(txn_id_t txn_id);
LogRecord MakeAbortRecord(txn_id_t txn_id);
LogRecord MakeInsertRecord(txn_id_t txn_id, page_id_t page_id,
                           std::string key, std::string value);
LogRecord MakeUpdateRecord(txn_id_t txn_id, page_id_t page_id,
                           std::string key, std::string old_value,
                           std::string new_value);
LogRecord MakeDeleteRecord(txn_id_t txn_id, page_id_t page_id,
                           std::string key, std::string old_value);

}  // namespace dbengine
