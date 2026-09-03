#include <cstdio>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "storage/log_manager.h"
#include "storage/log_record.h"
#include "txn/recovery_manager.h"

using dbengine::LogManager;
using dbengine::LogRecord;
using dbengine::LogRecordType;
using dbengine::MakeAbortRecord;
using dbengine::MakeBeginRecord;
using dbengine::MakeCommitRecord;
using dbengine::MakeDeleteRecord;
using dbengine::MakeInsertRecord;
using dbengine::MakeUpdateRecord;
using dbengine::page_id_t;
using dbengine::RecoveryManager;
using dbengine::RecoveryTarget;
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

// Mock RecoveryTarget for testing recovery in isolation
class MockRecoveryTarget : public RecoveryTarget {
 public:
  struct Action {
    std::string type;
    page_id_t page_id;
    std::string key;
    std::string value;
  };

  Status Redo(page_id_t page_id, const std::string& key,
              const std::string& after_image) override {
    actions.push_back({"REDO", page_id, key, after_image});
    if (after_image.empty()) {
      state.erase(key);
    } else {
      state[key] = after_image;
    }
    return Status::OK();
  }

  Status Undo(page_id_t page_id, const std::string& key,
              const std::string& before_image) override {
    actions.push_back({"UNDO", page_id, key, before_image});
    if (before_image.empty()) {
      state.erase(key);
    } else {
      state[key] = before_image;
    }
    return Status::OK();
  }

  std::vector<Action> actions;
  std::unordered_map<std::string, std::string> state;
};

void TestLogRecordCodec() {
  // Test Insert
  LogRecord r1 = MakeInsertRecord(10, 5, "hello", "world");
  r1.lsn = 100;
  std::string encoded1 = r1.Encode();
  LogRecord decoded1;
  Check(LogRecord::Decode(encoded1.data(), encoded1.size(), &decoded1),
        "decode insert record");
  Check(decoded1.txn_id == 10 && decoded1.page_id == 5 &&
            decoded1.type == LogRecordType::kInsert &&
            decoded1.key == "hello" && decoded1.after_image == "world",
        "decoded insert record matches original");

  // Test Update
  LogRecord r2 = MakeUpdateRecord(11, 6, "count", "1", "2");
  r2.lsn = 101;
  std::string encoded2 = r2.Encode();
  LogRecord decoded2;
  Check(LogRecord::Decode(encoded2.data(), encoded2.size(), &decoded2),
        "decode update record");
  Check(decoded2.txn_id == 11 && decoded2.before_image == "1" &&
            decoded2.after_image == "2",
        "decoded update record matches original");

  // Test Delete
  LogRecord r3 = MakeDeleteRecord(12, 7, "remove_me", "old_val");
  std::string encoded3 = r3.Encode();
  LogRecord decoded3;
  Check(LogRecord::Decode(encoded3.data(), encoded3.size(), &decoded3),
        "decode delete record");
  Check(decoded3.type == LogRecordType::kDelete && decoded3.before_image == "old_val" &&
            decoded3.after_image.empty(),
        "decoded delete record matches original");
}

void TestLogManagerAppendFlushIterate() {
  const char* log_file = "test_log_mgr.log";
  std::remove(log_file);
  {
    LogManager lm(log_file);
    LogRecord r1 = MakeBeginRecord(1);
    LogRecord r2 = MakeInsertRecord(1, 10, "k1", "v1");
    LogRecord r3 = MakeCommitRecord(1);

    Check(lm.Append(&r1).ok(), "append begin record");
    Check(lm.Append(&r2).ok(), "append insert record");
    Check(lm.AppendAndFlush(&r3).ok(), "append and flush commit record");

    Check(lm.GetFlushedLSN() >= r3.lsn, "flushed LSN advanced");

    std::vector<LogRecord> read_records;
    Status s = lm.IterateAll([&](const LogRecord& rec) {
      read_records.push_back(rec);
    });
    Check(s.ok(), "iterate all log records successfully");
    Check(read_records.size() == 3, "read back 3 log records");
    Check(read_records[0].type == LogRecordType::kBegin, "record 0 is Begin");
    Check(read_records[1].type == LogRecordType::kInsert, "record 1 is Insert");
    Check(read_records[2].type == LogRecordType::kCommit, "record 2 is Commit");
  }
  std::remove(log_file);
}

void TestRecoveryAnalysisRedoUndo() {
  const char* log_file = "test_recovery_mock.log";
  std::remove(log_file);
  {
    LogManager lm(log_file);

    // Txn 1: committed (inserts a, b)
    LogRecord r1 = MakeBeginRecord(1);
    LogRecord r2 = MakeInsertRecord(1, 100, "a", "alpha");
    LogRecord r3 = MakeInsertRecord(1, 100, "b", "bravo");
    LogRecord r4 = MakeCommitRecord(1);

    // Txn 2: active / uncommitted when crash occurs (inserts c, updates a)
    LogRecord r5 = MakeBeginRecord(2);
    LogRecord r6 = MakeInsertRecord(2, 101, "c", "charlie");
    LogRecord r7 = MakeUpdateRecord(2, 100, "a", "alpha", "alpha_mod");

    lm.Append(&r1);
    lm.Append(&r2);
    lm.Append(&r3);
    lm.Append(&r4);
    lm.Append(&r5);
    lm.Append(&r6);
    lm.Append(&r7);
    lm.Flush();
  }

  // Run Recovery against Mock Target
  {
    LogManager lm(log_file);
    MockRecoveryTarget target;
    RecoveryManager rm(&lm, &target);

    Status s = rm.Recover();
    Check(s.ok(), "recovery run successfully");

    // Check Dirty Page Table and Active Txns
    Check(rm.DirtyPageTable().count(100) == 1, "page 100 is in dirty page table");
    Check(rm.DirtyPageTable().count(101) == 1, "page 101 is in dirty page table");
    Check(rm.ActiveTxns().count(2) == 1, "txn 2 was active at crash time");
    Check(rm.ActiveTxns().count(1) == 0, "txn 1 was committed");

    // Verify Final Target State:
    // Txn 1 committed: "a" = "alpha", "b" = "bravo"
    // Txn 2 active: "c" inserted then undone (removed), "a" updated then undone (reverted to "alpha")
    Check(target.state["a"] == "alpha", "key 'a' reverted to committed value 'alpha'");
    Check(target.state["b"] == "bravo", "key 'b' has committed value 'bravo'");
    Check(target.state.count("c") == 0, "uncommitted key 'c' was rolled back");
  }
  std::remove(log_file);
}

}  // namespace

int main() {
  TestLogRecordCodec();
  TestLogManagerAppendFlushIterate();
  TestRecoveryAnalysisRedoUndo();

  if (g_failures == 0) {
    std::cout << "\nAll WAL & Recovery checks passed.\n";
    return 0;
  }
  std::cout << "\n" << g_failures << " check(s) failed.\n";
  return 1;
}
