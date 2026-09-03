#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "index/bplus_tree_engine.h"
#include "storage/buffer_pool_manager.h"
#include "storage/disk_manager.h"
#include "storage/log_manager.h"
#include "txn/lock_manager.h"
#include "txn/transaction.h"

using dbengine::BPlusTreeEngine;
using dbengine::BufferPoolManager;
using dbengine::DiskManager;
using dbengine::LockManager;
using dbengine::LogManager;
using dbengine::Status;
using dbengine::Transaction;
using dbengine::TransactionState;

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

void TestLockManagerBasic() {
  LockManager lm;
  Transaction t1(1);
  Transaction t2(2);

  // Both can acquire shared lock on same key
  Check(lm.AcquireShared(&t1, "shared_key").ok(), "T1 acquires shared lock");
  Check(lm.AcquireShared(&t2, "shared_key").ok(), "T2 acquires shared lock on same key");

  // Release T1, T2
  Check(lm.Unlock(&t1, "shared_key").ok(), "T1 releases shared lock");
  Check(lm.Unlock(&t2, "shared_key").ok(), "T2 releases shared lock");

  // 2PL violation test: T1 is now shrinking, cannot acquire new lock
  Check(t1.GetState() == TransactionState::kShrinking, "T1 is now in shrinking phase");
  Status s = lm.AcquireShared(&t1, "another_key");
  Check(!s.ok(), "T1 cannot acquire lock in shrinking phase (2PL enforced)");
}

void TestLockManagerMutualExclusion() {
  LockManager lm;
  Transaction t1(1);
  Transaction t2(2);

  Check(lm.AcquireExclusive(&t1, "exclusive_key").ok(), "T1 acquires exclusive lock");

  std::atomic<bool> t2_granted = false;
  std::thread th([&]() {
    lm.AcquireExclusive(&t2, "exclusive_key");
    t2_granted = true;
    lm.ReleaseAll(&t2);
  });

  // Small sleep to ensure T2 is blocked
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  Check(!t2_granted, "T2 is blocked waiting for exclusive lock held by T1");

  // Unlock T1 -> unblocks T2
  lm.ReleaseAll(&t1);
  th.join();
  Check(t2_granted, "T2 acquired exclusive lock after T1 released");
}

void TestTransactionCommitAndAbort() {
  const char* db_file = "test_txn_commit_abort.db";
  const char* log_file = "test_txn_commit_abort.log";
  std::remove(db_file);
  std::remove(log_file);
  {
    DiskManager dm(db_file);
    LogManager lm(log_file);
    BufferPoolManager bpm(10, &dm, &lm);
    BPlusTreeEngine engine(&bpm, &lm);

    // Initial base data
    engine.Put("user_1", "100");
    engine.Put("user_2", "200");

    // Transaction 1: Transfers 50 from user_1 to user_2, then commits
    auto txn1 = engine.BeginTransaction();
    std::string v1, v2;
    engine.Get(txn1.get(), "user_1", &v1);
    engine.Get(txn1.get(), "user_2", &v2);

    engine.Put(txn1.get(), "user_1", "50");
    engine.Put(txn1.get(), "user_2", "250");
    Check(engine.Commit(txn1.get()).ok(), "Txn 1 commits");

    std::string val1, val2;
    engine.Get("user_1", &val1);
    engine.Get("user_2", &val2);
    Check(val1 == "50" && val2 == "250", "committed values are visible in store");

    // Transaction 2: Tries to modify user_1 and user_2, but aborts
    auto txn2 = engine.BeginTransaction();
    engine.Put(txn2.get(), "user_1", "0");
    engine.Put(txn2.get(), "user_2", "500");
    engine.Put(txn2.get(), "user_3", "999");
    Check(engine.Abort(txn2.get()).ok(), "Txn 2 aborts");

    // Values should be rolled back
    engine.Get("user_1", &val1);
    engine.Get("user_2", &val2);
    Check(val1 == "50" && val2 == "250", "aborted values rolled back to 50 and 250");
    std::string val3;
    Check(engine.Get("user_3", &val3).IsNotFound(), "inserted key in aborted txn is removed");
  }
  std::remove(db_file);
  std::remove(log_file);
}

void TestConcurrentReadWriteStress() {
  const char* db_file = "test_concurrent_stress.db";
  const char* log_file = "test_concurrent_stress.log";
  std::remove(db_file);
  std::remove(log_file);
  {
    DiskManager dm(db_file);
    LogManager lm(log_file);
    BufferPoolManager bpm(20, &dm, &lm);
    BPlusTreeEngine engine(&bpm, &lm);

    constexpr int kNumThreads = 16;
    constexpr int kOpsPerThread = 10000;

    std::cout << "Starting high-CPU benchmark: threads=" << kNumThreads
              << ", ops_per_thread=" << kOpsPerThread
              << ", total_ops=" << (kNumThreads * kOpsPerThread) << "\n";

    std::vector<std::thread> workers;
    workers.reserve(kNumThreads);

    std::atomic<int> success_count = 0;

    for (int t = 0; t < kNumThreads; ++t) {
      workers.emplace_back([&, t]() {
        for (int op = 0; op < kOpsPerThread; ++op) {
          std::string key = "key_" + std::to_string((t * 37 + op) % 100);
          std::string val = "val_" + std::to_string(t) + "_" + std::to_string(op);

          if (op % 4 == 0) {
            // Transactional Put & Commit
            auto txn = engine.BeginTransaction();
            if (engine.Put(txn.get(), key, val).ok()) {
              if (engine.Commit(txn.get()).ok()) {
                ++success_count;
              }
            }
          } else if (op % 4 == 1) {
            // Transactional Put & Abort
            auto txn = engine.BeginTransaction();
            engine.Put(txn.get(), key, "temp_aborted");
            if (engine.Abort(txn.get()).ok()) {
              ++success_count;
            }
          } else if (op % 4 == 2) {
            // Direct Put
            if (engine.Put(key, val).ok()) {
              ++success_count;
            }
          } else {
            // Read
            std::string out;
            engine.Get(key, &out);
            ++success_count;
          }
        }
      });
    }

    for (auto& w : workers) {
      w.join();
    }

    Check(success_count == kNumThreads * kOpsPerThread,
          "all 1600 concurrent thread operations finished successfully");

    std::string error;
    Check(engine.ValidateInvariants(&error),
          "tree invariants intact after multi-threaded stress: " + error);
  }
  std::remove(db_file);
  std::remove(log_file);
}

}  // namespace

int main() {
  TestLockManagerBasic();
  TestLockManagerMutualExclusion();
  TestTransactionCommitAndAbort();
  TestConcurrentReadWriteStress();

  if (g_failures == 0) {
    std::cout << "\nAll Transaction & Concurrency checks passed.\n";
    return 0;
  }
  std::cout << "\n" << g_failures << " check(s) failed.\n";
  return 1;
}
