#include <cstdint>
#include <iostream>
#include <string>

#include "storage/log_manager.h"
#include "storage/log_record.h"

namespace {

const char* TypeName(dbengine::LogRecordType type) {
  switch (type) {
    case dbengine::LogRecordType::kBegin: return "BEGIN";
    case dbengine::LogRecordType::kCommit: return "COMMIT";
    case dbengine::LogRecordType::kAbort: return "ABORT";
    case dbengine::LogRecordType::kInsert: return "INSERT";
    case dbengine::LogRecordType::kUpdate: return "UPDATE";
    case dbengine::LogRecordType::kDelete: return "DELETE";
    case dbengine::LogRecordType::kCheckpoint: return "CHECKPOINT";
  }
  return "UNKNOWN";
}

void PrintRecord(const dbengine::LogRecord& record) {
  std::cout << "LSN " << record.lsn << "  " << TypeName(record.type)
            << "  txn=" << record.txn_id;
  if (record.page_id != dbengine::INVALID_PAGE_ID) {
    std::cout << "  page=" << record.page_id;
  }
  if (!record.key.empty()) {
    std::cout << "  key=" << record.key;
  }
  if (!record.before_image.empty()) {
    std::cout << "  before=" << record.before_image;
  }
  if (!record.after_image.empty()) {
    std::cout << "  after=" << record.after_image;
  }
  std::cout << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2 || argc > 3 || (argc == 3 && std::string(argv[2]) != "--full")) {
    std::cerr << "Usage: dump_wal <path_to_wal_file> [--full]\n";
    return 1;
  }

  const std::string wal_path = argv[1];
  const bool full = argc == 3;
  dbengine::LogManager log_manager(wal_path);
  std::uint64_t total = 0;
  std::uint64_t begins = 0;
  std::uint64_t commits = 0;
  std::uint64_t aborts = 0;
  std::uint64_t inserts = 0;
  std::uint64_t updates = 0;
  std::uint64_t deletes = 0;
  std::uint64_t checkpoints = 0;

  const dbengine::Status status = log_manager.IterateAll([&](const dbengine::LogRecord& record) {
    ++total;
    switch (record.type) {
      case dbengine::LogRecordType::kBegin: ++begins; break;
      case dbengine::LogRecordType::kCommit: ++commits; break;
      case dbengine::LogRecordType::kAbort: ++aborts; break;
      case dbengine::LogRecordType::kInsert: ++inserts; break;
      case dbengine::LogRecordType::kUpdate: ++updates; break;
      case dbengine::LogRecordType::kDelete: ++deletes; break;
      case dbengine::LogRecordType::kCheckpoint: ++checkpoints; break;
    }
    if (full) {
      PrintRecord(record);
    }
  });

  if (!status.ok()) {
    std::cerr << "WAL validation failed: " << status.message() << "\n";
    return 2;
  }

  std::cout << "WAL: " << wal_path << "\n"
            << "Total records: " << total << "\n"
            << "  BEGIN: " << begins << "\n"
            << "  INSERT: " << inserts << "\n"
            << "  UPDATE: " << updates << "\n"
            << "  DELETE: " << deletes << "\n"
            << "  COMMIT: " << commits << "\n"
            << "  ABORT: " << aborts << "\n"
            << "  CHECKPOINT: " << checkpoints << "\n";
  return 0;
}
