#include "storage/log_record.h"

#include <cstring>

namespace dbengine {

// Wire format (no LSN — assigned by LogManager at append time):
//   u8   type
//   u32  txn_id
//   i32  page_id
//   u32  key_len,  key bytes
//   u32  before_len, before bytes
//   u32  after_len,  after bytes
//
// All multi-byte integers are little-endian. Strings are length-prefixed,
// so empty strings round-trip cleanly.

namespace {

void AppendU8(std::string* buf, uint8_t v) { buf->push_back(static_cast<char>(v)); }

void AppendU32(std::string* buf, uint32_t v) {
  char bytes[4];
  std::memcpy(bytes, &v, 4);
  buf->append(bytes, 4);
}

void AppendI32(std::string* buf, int32_t v) {
  char bytes[4];
  std::memcpy(bytes, &v, 4);
  buf->append(bytes, 4);
}

void AppendString(std::string* buf, const std::string& s) {
  AppendU32(buf, static_cast<uint32_t>(s.size()));
  buf->append(s);
}

bool ReadU8(const char* data, size_t len, size_t* off, uint8_t* out) {
  if (*off + 1 > len) return false;
  *out = static_cast<uint8_t>(data[*off]);
  *off += 1;
  return true;
}

bool ReadU32(const char* data, size_t len, size_t* off, uint32_t* out) {
  if (*off + 4 > len) return false;
  std::memcpy(out, data + *off, 4);
  *off += 4;
  return true;
}

bool ReadI32(const char* data, size_t len, size_t* off, int32_t* out) {
  if (*off + 4 > len) return false;
  std::memcpy(out, data + *off, 4);
  *off += 4;
  return true;
}

bool ReadString(const char* data, size_t len, size_t* off, std::string* out) {
  uint32_t sz;
  if (!ReadU32(data, len, off, &sz)) return false;
  if (*off + sz > len) return false;
  out->assign(data + *off, sz);
  *off += sz;
  return true;
}

constexpr uint32_t kMaxFieldBytes = 64 * 1024 * 1024;  // 64MB sanity cap

}  // namespace

std::string LogRecord::Encode() const {
  std::string buf;
  AppendU8(&buf, static_cast<uint8_t>(type));
  AppendU32(&buf, txn_id);
  AppendI32(&buf, page_id);
  AppendString(&buf, key);
  AppendString(&buf, before_image);
  AppendString(&buf, after_image);
  return buf;
}

bool LogRecord::Decode(const char* data, size_t len, LogRecord* out) {
  size_t off = 0;
  uint8_t type_u8;
  if (!ReadU8(data, len, &off, &type_u8)) return false;
  if (type_u8 > static_cast<uint8_t>(LogRecordType::kCheckpoint)) return false;

  uint32_t txn;
  int32_t pid;
  if (!ReadU32(data, len, &off, &txn)) return false;
  if (!ReadI32(data, len, &off, &pid)) return false;

  std::string k, bef, aft;
  if (!ReadString(data, len, &off, &k)) return false;
  if (k.size() > kMaxFieldBytes) return false;
  if (!ReadString(data, len, &off, &bef)) return false;
  if (bef.size() > kMaxFieldBytes) return false;
  if (!ReadString(data, len, &off, &aft)) return false;
  if (aft.size() > kMaxFieldBytes) return false;
  if (off != len) return false;  // trailing garbage → malformed

  out->type = static_cast<LogRecordType>(type_u8);
  out->txn_id = txn;
  out->page_id = pid;
  out->key = std::move(k);
  out->before_image = std::move(bef);
  out->after_image = std::move(aft);
  return true;
}

LogRecord MakeBeginRecord(txn_id_t txn_id) {
  LogRecord r;
  r.type = LogRecordType::kBegin;
  r.txn_id = txn_id;
  return r;
}

LogRecord MakeCommitRecord(txn_id_t txn_id) {
  LogRecord r;
  r.type = LogRecordType::kCommit;
  r.txn_id = txn_id;
  return r;
}

LogRecord MakeAbortRecord(txn_id_t txn_id) {
  LogRecord r;
  r.type = LogRecordType::kAbort;
  r.txn_id = txn_id;
  return r;
}

LogRecord MakeInsertRecord(txn_id_t txn_id, page_id_t page_id,
                           std::string key, std::string value) {
  LogRecord r;
  r.type = LogRecordType::kInsert;
  r.txn_id = txn_id;
  r.page_id = page_id;
  r.key = std::move(key);
  r.after_image = std::move(value);
  return r;
}

LogRecord MakeUpdateRecord(txn_id_t txn_id, page_id_t page_id,
                           std::string key, std::string old_value,
                           std::string new_value) {
  LogRecord r;
  r.type = LogRecordType::kUpdate;
  r.txn_id = txn_id;
  r.page_id = page_id;
  r.key = std::move(key);
  r.before_image = std::move(old_value);
  r.after_image = std::move(new_value);
  return r;
}

LogRecord MakeDeleteRecord(txn_id_t txn_id, page_id_t page_id,
                           std::string key, std::string old_value) {
  LogRecord r;
  r.type = LogRecordType::kDelete;
  r.txn_id = txn_id;
  r.page_id = page_id;
  r.key = std::move(key);
  r.before_image = std::move(old_value);
  return r;
}

}  // namespace dbengine
