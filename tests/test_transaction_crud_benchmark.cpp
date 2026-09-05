#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "index/bplus_tree_engine.h"
#include "storage/buffer_pool_manager.h"
#include "storage/disk_manager.h"
#include "storage/log_manager.h"

namespace {

using Clock = std::chrono::steady_clock;

struct ThreadResult {
  std::vector<double> latencies_ms;

  int committed = 0;

  int not_found = 0;

  int other_errors = 0;

  int inserts = 0;
  int updates = 0;
  int deletes = 0;
};

struct BenchmarkResult {
  int threads = 0;

  int total_operations = 0;

  int committed = 0;

  int not_found = 0;

  int other_errors = 0;

  int inserts = 0;
  int updates = 0;
  int deletes = 0;

  double elapsed_seconds = 0.0;

  double tps = 0.0;

  double p50_ms = 0.0;
  double p95_ms = 0.0;
  double p99_ms = 0.0;
};

class StartGate {
 public:
  explicit StartGate(int thread_count)
      : thread_count_(thread_count) {}

  void Wait() {
    std::unique_lock<std::mutex> lock(mutex_);

    ++ready_;

    if (ready_ == thread_count_) {
      start_ = true;
      cv_.notify_all();
    } else {
      cv_.wait(lock, [&] {
        return start_;
      });
    }
  }

 private:
  int thread_count_;
  int ready_ = 0;
  bool start_ = false;

  std::mutex mutex_;
  std::condition_variable cv_;
};

double Percentile(std::vector<double> values,
                  double percentile) {

  if (values.empty()) {
    return 0.0;
  }

  std::sort(values.begin(), values.end());

  const double position =
      (percentile / 100.0) *
      (values.size() - 1);

  const size_t lower =
      static_cast<size_t>(position);

  const size_t upper =
      std::min(
          lower + 1,
          values.size() - 1);

  const double fraction =
      position - lower;

  return values[lower] +
         fraction *
             (values[upper] -
              values[lower]);
}

bool CreateInitialDatabase(
    const std::string& db_file,
    const std::string& wal_file,
    int key_count) {

  dbengine::DiskManager disk(db_file);
  dbengine::LogManager wal(wal_file);

  dbengine::BufferPoolManager bpm(
      64,
      &disk,
      &wal);

  dbengine::BPlusTreeEngine engine(
      &bpm,
      &wal,
      true);

  /*
   * Start with exactly 500 existing keys.
   */
  for (int key_number = 1;
       key_number <= key_count;
       ++key_number) {

    const std::string key =
        "user:" +
        std::to_string(key_number);

    const std::string value =
        "initial:" +
        std::to_string(key_number);

    auto transaction =
        engine.BeginTransaction();

    if (!engine.Put(
            transaction.get(),
            key,
            value)
             .ok()) {

      engine.Abort(transaction.get());
      return false;
    }

    if (!engine.Commit(
            transaction.get())
             .ok()) {

      engine.Abort(transaction.get());
      return false;
    }
  }

  return true;
}

BenchmarkResult RunBenchmark(
    const std::string& db_file,
    const std::string& wal_file,
    int thread_count,
    int operations_per_thread,
    int key_count) {

  /*
   * Fresh database for every concurrency level.
   */
  if (std::remove(db_file.c_str()) != 0) {
    // File may not exist.
  }

  if (std::remove(wal_file.c_str()) != 0) {
    // File may not exist.
  }

  if (!CreateInitialDatabase(
          db_file,
          wal_file,
          key_count)) {

    std::cerr
        << "Failed to initialize database\n";

    return {};
  }

  dbengine::DiskManager disk(db_file);
  dbengine::LogManager wal(wal_file);

  dbengine::BufferPoolManager bpm(
      64,
      &disk,
      &wal);

  dbengine::BPlusTreeEngine engine(
      &bpm,
      &wal,
      true);

  std::vector<ThreadResult> thread_results(
      thread_count);

  StartGate start_gate(thread_count);

  std::vector<std::thread> workers;

  workers.reserve(thread_count);

  const auto benchmark_start =
      Clock::now();

  for (int thread_id = 0;
       thread_id < thread_count;
       ++thread_id) {

    workers.emplace_back(
        [&, thread_id]() {

          ThreadResult& result =
              thread_results[thread_id];

          result.latencies_ms.reserve(
              operations_per_thread);

          start_gate.Wait();

          for (int operation = 0;
               operation < operations_per_thread;
               ++operation) {

            /*
             * Keep all workload keys in:
             *
             * user:1 -> user:500
             */
            const int key_number =
                (thread_id + operation) %
                    key_count +
                1;

            const std::string key =
                "user:" +
                std::to_string(key_number);

            const std::string value =
                "thread:" +
                std::to_string(thread_id) +
                ":operation:" +
                std::to_string(operation);

            const auto txn_start =
                Clock::now();

            auto transaction =
                engine.BeginTransaction();

            /*
             * Workload:
             *
             * 0 -> UPDATE
             * 1 -> DELETE
             * 2 -> INSERT/UPSERT
             *
             * Put() is used for both insert and
             * update because your engine implements
             * Put() as an upsert.
             */
            const int operation_type =
                (thread_id + operation) % 3;

            bool operation_ok = false;

            if (operation_type == 0) {

              // UPDATE
              //
              // If key exists, Put() changes its value.
              operation_ok =
                  engine.Put(
                      transaction.get(),
                      key,
                      value)
                      .ok();

              if (operation_ok) {
                ++result.updates;
              }

            } else if (operation_type == 1) {

              // DELETE

              const auto status =
                  engine.Delete(
                      transaction.get(),
                      key);

              operation_ok =
                  status.ok();

              if (operation_ok) {
                ++result.deletes;
              } else {

                /*
                 * NotFound is an expected result in
                 * this concurrent CRUD workload.
                 *
                 * We do NOT call this a database
                 * failure.
                 */
                ++result.not_found;
              }

            } else {

              // INSERT / UPSERT

              operation_ok =
                  engine.Put(
                      transaction.get(),
                      key,
                      value)
                      .ok();

              if (operation_ok) {
                ++result.inserts;
              }
            }

            if (!operation_ok) {

              /*
               * The operation itself failed.
               *
               * For Delete(NotFound), this is an
               * expected workload result.
               */
              engine.Abort(
                  transaction.get());

            } else {

              const auto commit_status =
                  engine.Commit(
                      transaction.get());

              if (!commit_status.ok()) {

                ++result.other_errors;

                engine.Abort(
                    transaction.get());

              } else {

                ++result.committed;
              }
            }

            const auto txn_end =
                Clock::now();

            const double latency_ms =
                std::chrono::duration<double,
                    std::milli>(
                    txn_end -
                    txn_start)
                    .count();

            result.latencies_ms.push_back(
                latency_ms);
          }
        });
  }

  for (auto& worker : workers) {
    worker.join();
  }

  const auto benchmark_end =
      Clock::now();

  BenchmarkResult result;

  result.threads = thread_count;

  result.total_operations =
      thread_count *
      operations_per_thread;

  for (const auto& thread_result :
       thread_results) {

    result.committed +=
        thread_result.committed;

    result.not_found +=
        thread_result.not_found;

    result.other_errors +=
        thread_result.other_errors;

    result.inserts +=
        thread_result.inserts;

    result.updates +=
        thread_result.updates;

    result.deletes +=
        thread_result.deletes;
  }

  result.elapsed_seconds =
      std::chrono::duration<double>(
          benchmark_end -
          benchmark_start)
          .count();

  if (result.elapsed_seconds > 0.0) {

    result.tps =
        result.committed /
        result.elapsed_seconds;
  }

  std::vector<double> latencies;

  latencies.reserve(
      result.total_operations);

  for (const auto& thread_result :
       thread_results) {

    latencies.insert(
        latencies.end(),
        thread_result.latencies_ms.begin(),
        thread_result.latencies_ms.end());
  }

  result.p50_ms =
      Percentile(latencies, 50.0);

  result.p95_ms =
      Percentile(latencies, 95.0);

  result.p99_ms =
      Percentile(latencies, 99.0);

  return result;
}

}  // namespace

int main(int argc, char** argv) {

  const std::string db_file =
      argc > 1
          ? argv[1]
          : "benchmark_crud.db";

  const std::string wal_file =
      argc > 2
          ? argv[2]
          : "benchmark_crud.wal";

  const int operations_per_thread =
      argc > 3
          ? std::atoi(argv[3])
          : 1000;

  const int key_count =
      argc > 4
          ? std::atoi(argv[4])
          : 500;

  if (operations_per_thread <= 0 ||
      key_count <= 0 ||
      key_count > 500) {

    std::cerr
        << "Usage:\n"
        << "  transaction_crud_benchmark "
        << "[db] [wal] "
        << "[ops_per_thread] "
        << "[key_count]\n";

    return 1;
  }

  const std::vector<int> thread_counts = {
      1, 2, 4, 8, 16
  };

  std::cout
      << "\nTransaction CRUD concurrency benchmark\n"
      << "Operations per thread: "
      << operations_per_thread << "\n"
      << "Keys: 1-" << key_count << "\n"
      << "Workload: UPDATE / DELETE / UPSERT\n\n";

  std::vector<BenchmarkResult> results;

  for (int threads : thread_counts) {

    std::cout
        << "Testing "
        << threads
        << " thread"
        << (threads == 1 ? "" : "s")
        << "...\n";

    BenchmarkResult result =
        RunBenchmark(
            db_file,
            wal_file,
            threads,
            operations_per_thread,
            key_count);

    results.push_back(result);

    std::cout
        << "  TPS: "
        << std::fixed
        << std::setprecision(2)
        << result.tps << "\n"

        << "  p50: "
        << result.p50_ms
        << " ms\n"

        << "  p95: "
        << result.p95_ms
        << " ms\n"

        << "  p99: "
        << result.p99_ms
        << " ms\n"

        << "  Committed: "
        << result.committed << "\n"

        << "  Expected NotFound: "
        << result.not_found << "\n"

        << "  Other errors: "
        << result.other_errors << "\n\n";
  }

  std::cout
      << "\n==============================================================\n";

  std::cout
      << std::left
      << std::setw(10)
      << "Threads"

      << std::right
      << std::setw(10)
      << "TPS"

      << std::setw(12)
      << "p50(ms)"

      << std::setw(12)
      << "p95(ms)"

      << std::setw(12)
      << "p99(ms)"

      << std::setw(12)
      << "Commit"

      << std::setw(12)
      << "NotFound"

      << std::setw(12)
      << "Errors"

      << "\n";

  std::cout
      << "--------------------------------------------------------------\n";

  for (const auto& result : results) {

    std::cout
        << std::left
        << std::setw(10)
        << result.threads

        << std::right
        << std::fixed
        << std::setprecision(2)

        << std::setw(10)
        << result.tps

        << std::setw(12)
        << result.p50_ms

        << std::setw(12)
        << result.p95_ms

        << std::setw(12)
        << result.p99_ms

        << std::setw(12)
        << result.committed

        << std::setw(12)
        << result.not_found

        << std::setw(12)
        << result.other_errors

        << "\n";
  }

  std::cout
      << "==============================================================\n";

  return 0;
}