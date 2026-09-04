#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "index/bplus_tree_engine.h"
#include "storage/buffer_pool_manager.h"
#include "storage/disk_manager.h"
#include "storage/log_manager.h"

int main(int argc, char** argv) {
  const std::string db_file = argc > 1 ? argv[1] : "dbengine.db";
  const std::string wal_file = argc > 2 ? argv[2] : "dbengine.wal";
  const int thread_count = argc > 3 ? std::atoi(argv[3]) : 8;
  const int operations_per_thread = argc > 4 ? std::atoi(argv[4]) : 100;
  const int key_count = argc > 5 ? std::atoi(argv[5]) : 450;

  if (thread_count <= 0 || operations_per_thread <= 0 ||
      key_count <= 0 || key_count > 450) {
    std::cerr
        << "Usage: transaction_concurrency_benchmark "
        << "[db] [wal] [threads] [ops_per_thread] [key_count]\n"
        << "key_count must be between 1 and 450\n";
    return 1;
  }

  dbengine::DiskManager disk(db_file);
  dbengine::LogManager wal(wal_file);
  dbengine::BufferPoolManager bpm(64, &disk, &wal);
  dbengine::BPlusTreeEngine engine(&bpm, &wal, true);

  std::atomic<int> committed{0};
  std::atomic<int> failed{0};

  std::atomic<int> inserts{0};
  std::atomic<int> updates{0};
  std::atomic<int> deletes{0};

  const auto started = std::chrono::steady_clock::now();

  std::vector<std::thread> workers;
  workers.reserve(thread_count);

  for (int thread_id = 0; thread_id < thread_count; ++thread_id) {
    workers.emplace_back([&, thread_id]() {
      for (int operation = 0;
           operation < operations_per_thread;
           ++operation) {

        // Keys are ALWAYS restricted to:
        //
        // user:1
        // user:2
        // ...
        // user:450
        //
        const int key_number =
            (thread_id + operation) % key_count + 1;

        const std::string key =
            "user:" + std::to_string(key_number);

        const std::string value =
            "thread:" + std::to_string(thread_id) +
            ":operation:" + std::to_string(operation);

        auto transaction = engine.BeginTransaction();

        bool operation_ok = false;

        // Mix INSERT, UPDATE and DELETE operations.
        //
        // Put() is an upsert:
        //   - If key doesn't exist -> INSERT
        //   - If key exists       -> UPDATE
        //
        // Delete() removes an existing key.

        const int operation_type =
            (thread_id + operation) % 3;

        if (operation_type == 0) {
          // INSERT / UPSERT
          //
          // Put() may either insert or update depending on whether
          // the key already exists.
          //
          // We count this as an insert/upsert workload operation.
          operation_ok =
              engine.Put(transaction.get(), key, value).ok();

          if (operation_ok) {
            ++inserts;
          }

        } else if (operation_type == 1) {
          // UPDATE
          //
          // Put() updates the value if the key already exists.
          // It also inserts if the key was deleted by another
          // transaction, which is normal for an upsert API.
          operation_ok =
              engine.Put(transaction.get(), key, value).ok();

          if (operation_ok) {
            ++updates;
          }

        } else {
          // DELETE
          operation_ok =
              engine.Delete(transaction.get(), key).ok();

          if (operation_ok) {
            ++deletes;
          }
        }

        if (!operation_ok ||
            !engine.Commit(transaction.get()).ok()) {
          engine.Abort(transaction.get());
          ++failed;
          continue;
        }

        ++committed;
      }
    });
  }

  for (auto& worker : workers) {
    worker.join();
  }

  const auto elapsed = std::chrono::duration<double>(
      std::chrono::steady_clock::now() - started).count();

  // Verify that the database can still be accessed after
  // all concurrent transactions have completed.
  int final_verifications = 0;

  for (int key_number = 1;
       key_number <= key_count;
       ++key_number) {

    std::string stored_value;

    if (engine.Get(
            "user:" + std::to_string(key_number),
            &stored_value).ok()) {
      ++final_verifications;
    }
  }

  std::string invariant_error;

  const bool invariants_ok =
      engine.ValidateInvariants(&invariant_error);

  const int total_operations =
      thread_count * operations_per_thread;

  std::cout
      << "Transaction concurrency benchmark\n"
      << "Database: " << db_file << "\n"
      << "WAL: " << wal_file << "\n"
      << "Threads: " << thread_count << "\n"
      << "Operations per thread: "
      << operations_per_thread << "\n"
      << "Total operations: "
      << total_operations << "\n"
      << "Key range: 1-" << key_count << "\n"
      << "Committed: " << committed.load() << "\n"
      << "Failed: " << failed.load() << "\n"
      << "Insert/Upsert operations: "
      << inserts.load() << "\n"
      << "Update operations: "
      << updates.load() << "\n"
      << "Delete operations: "
      << deletes.load() << "\n"
      << "Final key verifications: "
      << final_verifications << "/" << key_count << "\n"
      << "Elapsed seconds: "
      << elapsed << "\n"
      << "Operations/second: "
      << (elapsed > 0.0
              ? total_operations / elapsed
              : 0.0)
      << "\n"
      << "Tree invariants: "
      << (invariants_ok ? "PASS" : "FAIL")
      << "\n";

  if (!invariants_ok) {
    std::cout
        << "Invariant error: "
        << invariant_error
        << "\n";
  }

  return failed == 0 &&
                 invariants_ok
             ? 0
             : 2;
}