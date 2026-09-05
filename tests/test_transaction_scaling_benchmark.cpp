#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
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
using Microseconds = std::chrono::microseconds;

struct ThreadResult {
  std::vector<double> latencies_ms;
  int committed = 0;
  int failed = 0;
};

struct BenchmarkResult {
  int threads = 0;
  int total_operations = 0;
  int committed = 0;
  int failed = 0;

  double elapsed_seconds = 0.0;
  double tps = 0.0;

  double p50_ms = 0.0;
  double p95_ms = 0.0;
  double p99_ms = 0.0;
};

double Percentile(std::vector<double> values, double percentile) {
  if (values.empty()) {
    return 0.0;
  }

  std::sort(values.begin(), values.end());

  const double position =
      (percentile / 100.0) * (values.size() - 1);

  const size_t lower =
      static_cast<size_t>(position);

  const size_t upper =
      std::min(lower + 1, values.size() - 1);

  const double fraction = position - lower;

  return values[lower] +
         fraction * (values[upper] - values[lower]);
}

class StartGate {
 public:
  explicit StartGate(int thread_count)
      : thread_count_(thread_count) {}

  void ArriveAndWait() {
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

BenchmarkResult RunBenchmark(
    dbengine::BPlusTreeEngine& engine,
    int thread_count,
    int operations_per_thread,
    int key_count) {

  std::vector<ThreadResult> results(thread_count);

  StartGate start_gate(thread_count);

  std::vector<std::thread> workers;
  workers.reserve(thread_count);

  const auto benchmark_start = Clock::now();

  for (int thread_id = 0;
       thread_id < thread_count;
       ++thread_id) {

    workers.emplace_back(
        [&, thread_id]() {

          ThreadResult& result =
              results[thread_id];

          result.latencies_ms.reserve(
              operations_per_thread);

          // Make all threads start their workload
          // at approximately the same time.
          start_gate.ArriveAndWait();

          for (int operation = 0;
               operation < operations_per_thread;
               ++operation) {

            /*
             * Keep all keys inside:
             *
             * user:1
             * ...
             * user:500
             *
             * With many more operations than keys,
             * multiple transactions will contend
             * for the same keys.
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

            const auto txn_start = Clock::now();

            auto transaction =
                engine.BeginTransaction();

            bool operation_ok = false;

            /*
             * Three-way workload:
             *
             * 0 -> Put()
             * 1 -> Put()
             * 2 -> Delete()
             *
             * Put() is an UPSERT:
             *
             *   missing key -> INSERT
             *   existing key -> UPDATE
             *
             * Therefore the benchmark exercises both
             * insertion and update paths through Put().
             */
            const int operation_type =
                (thread_id + operation) % 3;

            if (operation_type == 0 ||
                operation_type == 1) {

              operation_ok =
                  engine.Put(
                      transaction.get(),
                      key,
                      value).ok();

            } else {

              operation_ok =
                  engine.Delete(
                      transaction.get(),
                      key).ok();
            }

            if (!operation_ok) {
              engine.Abort(transaction.get());

              ++result.failed;

              const auto txn_end = Clock::now();

              const double latency =
                  std::chrono::duration<double,
                      std::milli>(
                          txn_end - txn_start)
                      .count();

              result.latencies_ms.push_back(
                  latency);

              continue;
            }

            const auto commit_status =
                engine.Commit(
                    transaction.get());

            if (!commit_status.ok()) {
              engine.Abort(transaction.get());

              ++result.failed;
            } else {
              ++result.committed;
            }

            const auto txn_end = Clock::now();

            const double latency =
                std::chrono::duration<double,
                    std::milli>(
                    txn_end - txn_start)
                    .count();

            result.latencies_ms.push_back(
                latency);
          }
        });
  }

  for (auto& worker : workers) {
    worker.join();
  }

  const auto benchmark_end = Clock::now();

  BenchmarkResult result;

  result.threads = thread_count;
  result.total_operations =
      thread_count * operations_per_thread;

  for (const auto& thread_result : results) {

    result.committed +=
        thread_result.committed;

    result.failed +=
        thread_result.failed;
  }

  result.elapsed_seconds =
      std::chrono::duration<double>(
          benchmark_end - benchmark_start)
          .count();

  if (result.elapsed_seconds > 0.0) {
    result.tps =
        result.committed /
        result.elapsed_seconds;
  }

  // Combine all transaction latencies.
  std::vector<double> all_latencies;

  all_latencies.reserve(
      result.total_operations);

  for (const auto& thread_result : results) {

    all_latencies.insert(
        all_latencies.end(),
        thread_result.latencies_ms.begin(),
        thread_result.latencies_ms.end());
  }

  result.p50_ms =
      Percentile(all_latencies, 50.0);

  result.p95_ms =
      Percentile(all_latencies, 95.0);

  result.p99_ms =
      Percentile(all_latencies, 99.0);

  return result;
}

}  // namespace

int main(int argc, char** argv) {

  const std::string db_file =
      argc > 1
          ? argv[1]
          : "dbengine.db";

  const std::string wal_file =
      argc > 2
          ? argv[2]
          : "dbengine.wal";

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
        << "  transaction_scaling_benchmark "
        << "[db] [wal] "
        << "[ops_per_thread] "
        << "[key_count]\n\n"
        << "Example:\n"
        << "  transaction_scaling_benchmark "
        << "./dbengine.db "
        << "./dbengine.wal "
        << "1000 "
        << "500\n";

    return 1;
  }

  /*
   * Your normal database stack.
   */
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
   * These are the concurrency levels we want
   * to compare.
   *
   * Your machine has 4 physical cores /
   * 8 logical processors, so testing 16 is
   * useful for observing contention.
   */
  const std::vector<int> thread_counts = {
      1, 2, 4, 8, 16
  };

  std::vector<BenchmarkResult> results;

  std::cout
      << "\nTransaction concurrency scaling benchmark\n"
      << "Database: " << db_file << "\n"
      << "WAL: " << wal_file << "\n"
      << "Operations per thread: "
      << operations_per_thread << "\n"
      << "Key range: user:1 -> user:"
      << key_count << "\n\n";

  std::cout
      << "Running benchmarks...\n\n";

  for (int thread_count : thread_counts) {

    std::cout
        << "Testing "
        << thread_count
        << " thread"
        << (thread_count == 1 ? "" : "s")
        << "...\n";

    BenchmarkResult result =
        RunBenchmark(
            engine,
            thread_count,
            operations_per_thread,
            key_count);

    results.push_back(result);

    std::cout
        << "  TPS: "
        << std::fixed
        << std::setprecision(2)
        << result.tps
        << "\n"

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
        << result.committed
        << "\n"

        << "  Failed: "
        << result.failed
        << "\n\n";
  }

  /*
   * Final summary table.
   */
  std::cout
      << "\n============================================================\n";

  std::cout
      << std::left
      << std::setw(10)
      << "Threads"
      << std::right
      << std::setw(12)
      << "TPS"
      << std::setw(12)
      << "p50 (ms)"
      << std::setw(12)
      << "p95 (ms)"
      << std::setw(12)
      << "p99 (ms)"
      << "\n";

  std::cout
      << "------------------------------------------------------------\n";

  for (const auto& result : results) {

    std::cout
        << std::left
        << std::setw(10)
        << result.threads

        << std::right
        << std::setw(12)
        << std::fixed
        << std::setprecision(2)
        << result.tps

        << std::setw(12)
        << result.p50_ms

        << std::setw(12)
        << result.p95_ms

        << std::setw(12)
        << result.p99_ms

        << "\n";
  }

  std::cout
      << "============================================================\n";

  /*
   * Check the B+ tree after all tests.
   */
  std::string invariant_error;

  const bool invariants_ok =
      engine.ValidateInvariants(
          &invariant_error);

  std::cout
      << "\nTree invariants: "
      << (invariants_ok ? "PASS" : "FAIL")
      << "\n";

  if (!invariants_ok) {
    std::cout
        << "Invariant error: "
        << invariant_error
        << "\n";
  }

  return invariants_ok ? 0 : 2;
}