#include <cstdio>
#include <iostream>
#include <string>

#include "index/bplus_tree_engine.h"
#include "storage/buffer_pool_manager.h"
#include "storage/disk_manager.h"
#include "storage/log_manager.h"

using dbengine::BPlusTreeEngine;
using dbengine::BufferPoolManager;
using dbengine::DiskManager;
using dbengine::LogManager;
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

void TestKillAndRecover() {
  const char* db_file = "test_kill_recover.db";
  const char* log_file = "test_kill_recover.log";
  std::remove(db_file);
  std::remove(log_file);

  // Phase 1: Run workload and simulate crash
  {
    DiskManager dm(db_file);
    LogManager lm(log_file);
    BufferPoolManager bpm(10, &dm, &lm);
    BPlusTreeEngine engine(&bpm, &lm, /*recover_on_open=*/false);

    // Txn 1: Inserts k1, k2, k3 -> Commits
    auto t1 = engine.BeginTransaction();
    engine.Put(t1.get(), "k1", "val1_initial");
    engine.Put(t1.get(), "k2", "val2_initial");
    engine.Put(t1.get(), "k3", "val3_initial");
    Check(engine.Commit(t1.get()).ok(), "Txn 1 commits");

    // Txn 2: Updates k1, inserts k4 -> Commits
    auto t2 = engine.BeginTransaction();
    engine.Put(t2.get(), "k1", "val1_updated_by_t2");
    engine.Put(t2.get(), "k4", "val4_initial");
    Check(engine.Commit(t2.get()).ok(), "Txn 2 commits");

    // Txn 3: Updates k2, inserts k5, k6 -> Uncommitted (Crash happens while active!)
    auto t3 = engine.BeginTransaction();
    engine.Put(t3.get(), "k2", "val2_uncommitted_t3");
    engine.Put(t3.get(), "k5", "val5_uncommitted_t3");
    engine.Put(t3.get(), "k6", "val6_uncommitted_t3");
    // Note: t3 is NOT committed! WAL contains Begin + Inserts/Updates, but no Commit.
    lm.Flush(); // Ensure log records reached the log file before crash

    // Txn 4: Inserts k7 -> Aborts
    auto t4 = engine.BeginTransaction();
    engine.Put(t4.get(), "k7", "val7_aborted");
    Check(engine.Abort(t4.get()).ok(), "Txn 4 aborts");

    // Simulate crash: no graceful MarkCleanShutdown, engine goes out of scope
  }

  // Phase 2: Reopen engine with ARIES-lite recovery enabled
  {
    DiskManager dm(db_file);
    LogManager lm(log_file);
    BufferPoolManager bpm(10, &dm, &lm);
    // recover_on_open runs RecoveryManager:
    // AnalysisPass -> identifies active Txn 3
    // RedoPass -> replays Txn 1, 2, 3 changes
    // UndoPass -> rolls back active Txn 3 changes
    BPlusTreeEngine engine(&bpm, &lm, /*recover_on_open=*/true);

    std::string val;

    // Verify Txn 1 & Txn 2 committed data survived
    Check(engine.Get("k1", &val).ok() && val == "val1_updated_by_t2",
          "k1 has committed value from Txn 2");
    Check(engine.Get("k3", &val).ok() && val == "val3_initial",
          "k3 has committed value from Txn 1");
    Check(engine.Get("k4", &val).ok() && val == "val4_initial",
          "k4 has committed value from Txn 2");

    // Verify Txn 3 (uncommitted at crash) was rolled back by Undo
    Check(engine.Get("k2", &val).ok() && val == "val2_initial",
          "k2 was rolled back to Txn 1 value (Txn 3 undone)");
    Check(engine.Get("k5", &val).IsNotFound(),
          "uncommitted k5 from Txn 3 is absent after recovery");
    Check(engine.Get("k6", &val).IsNotFound(),
          "uncommitted k6 from Txn 3 is absent after recovery");

    // Verify Txn 4 (aborted before crash) is absent
    Check(engine.Get("k7", &val).IsNotFound(),
          "aborted k7 from Txn 4 is absent after recovery");

    std::string error;
    Check(engine.ValidateInvariants(&error),
          "tree invariants valid after crash recovery: " + error);
  }

  std::remove(db_file);
  std::remove(log_file);
}

}  // namespace

int main() {
  TestKillAndRecover();

  if (g_failures == 0) {
    std::cout << "\nAll Crash Recovery checks passed.\n";
    return 0;
  }
  std::cout << "\n" << g_failures << " check(s) failed.\n";
  return 1;
}
