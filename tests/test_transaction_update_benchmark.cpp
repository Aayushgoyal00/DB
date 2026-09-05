#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
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
      std::min(lower + 1,
               values.size() - 1);

  const double fraction =
      position - lower;

  return values[lower] +
         fraction *
             (values[upper] - values[lower]);
}

bool CreateInitialDatabase(
    const std::string& db_file,
    const std::string& wal_file,
    int key_count) {

  // Create fresh storage files.
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
   * Populate:
   *
   * user:1
   * user:2
   * ...
   * user:<key_count>
   *
   * These keys will already exist when the
   * concurrency benchmark starts.
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

bool CopySeedFiles(
    const std::string& seed_db_file,
    const std::string& seed_wal_file,
    const std::string& db_file,
    const std::string& wal_file) {

  namespace fs = std::filesystem;

  std::error_code ec;

  /*
   * Wipe any leftover working files from a previous
   * thread-count iteration before copying the seed
   * back in.
   */
  fs::remove(db_file, ec);
  fs::remove(wal_file, ec);

  fs::copy_file(
      seed_db_file,
      db_file,
      fs::copy_options::overwrite_existing,
      ec);

  if (ec) {
    std::cerr
        << "Failed to copy seed db: "
        << ec.message() << "\n";
    return false;
  }

  fs::copy_file(
      seed_wal_file,
      wal_file,
      fs::copy_options::overwrite_existing,
      ec);

  if (ec) {
    std::cerr
        << "Failed to copy seed wal: "
        << ec.message() << "\n";
    return false;
  }

  return true;
}

BenchmarkResult RunBenchmark(
    const std::string& seed_db_file,
    const std::string& seed_wal_file,
    const std::string& db_file,
    const std::string& wal_file,
    int thread_count,
    int operations_per_thread,
    int key_count) {

  /*
   * IMPORTANT:
   *
   * Every thread-count test still starts from an
   * identical, freshly-populated database — but instead
   * of re-running key_count inserts (and their fsync'd
   * commits) on every single iteration, we just copy the
   * already-built seed files. Six thread-count runs used
   * to mean six full re-population passes; now it's one
   * population pass plus six cheap file copies.
   */
  if (!CopySeedFiles(
          seed_db_file,
          seed_wal_file,
          db_file,
          wal_file)) {

    std::cerr
        << "Failed to prepare database from seed\n";

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

          /*
           * Make all worker threads start
           * together.
           */
          start_gate.Wait();

          for (int operation = 0;
               operation < operations_per_thread;
               ++operation) {

            /*
             * ONLY keys 1-key_count.
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
                ":update:" +
                std::to_string(operation);

            const auto txn_start =
                Clock::now();

            auto transaction =
                engine.BeginTransaction();

            /*
             * Because the database was initialized
             * with all key_count keys, this Put()
             * should exercise the UPDATE path.
             */
            const auto put_status =
                engine.Put(
                    transaction.get(),
                    key,
                    value);

            bool success = put_status.ok();

            if (!success) {
              engine.Abort(
                  transaction.get());

              ++result.failed;
            } else {

              const auto commit_status =
                  engine.Commit(
                      transaction.get());

              if (!commit_status.ok()) {
                engine.Abort(
                    transaction.get());

                ++result.failed;
              } else {
                ++result.committed;
              }
            }

            const auto txn_end =
                Clock::now();

            const double latency_ms =
                std::chrono::duration<double,
                    std::milli>(
                    txn_end - txn_start)
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

    result.failed +=
        thread_result.failed;
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
          : "benchmark_update.db";

  const std::string wal_file =
      argc > 2
          ? argv[2]
          : "benchmark_update.wal";

  /*
   * This is now a FIXED TOTAL, not ops-per-thread.
   * Every thread-count configuration below executes
   * exactly this many transactions in total, split
   * evenly across however many worker threads are
   * running. That keeps "how much work is done" constant
   * while only concurrency varies, which is what makes
   * TPS across different thread counts comparable.
   */
  const int total_operations =
      argc > 3
          ? std::atoi(argv[3])
          : 16000;

  const int key_count =
      argc > 4
          ? std::atoi(argv[4])
          : 500;

  if (total_operations <= 0 ||
      key_count <= 0 ||
      key_count > 200000) {

    std::cerr
        << "Usage:\n"
        << "  transaction_update_benchmark "
        << "[db] [wal] "
        << "[total_operations] "
        << "[key_count]\n"
        << "key_count must be between 1 and 200000\n";

    return 1;
  }

  const std::vector<int> thread_counts = {
      1, 2, 4, 8, 16, 32
  };

  const std::string seed_db_file = db_file + ".seed";
  const std::string seed_wal_file = wal_file + ".seed";

  std::cout
      << "\nConcurrent UPDATE benchmark\n"
      << "Total operations per thread-count: "
      << total_operations << "\n"
      << "Keys: 1-" << key_count << "\n\n";

  std::cout
      << "Building seed database ("
      << key_count
      << " keys, once)...\n";

  std::remove(seed_db_file.c_str());
  std::remove(seed_wal_file.c_str());

  if (!CreateInitialDatabase(
          seed_db_file,
          seed_wal_file,
          key_count)) {

    std::cerr
        << "Failed to build seed database\n";

    return 1;
  }

  std::cout << "Seed database ready.\n\n";

  std::vector<BenchmarkResult> results;

  for (int threads : thread_counts) {

    /*
     * Fixed total, split evenly. Integer division means
     * a small remainder (total_operations % threads) is
     * dropped rather than executed, which keeps every
     * thread's workload identical; the effect on total
     * ops is negligible at these scales.
     */
    const int operations_per_thread =
        total_operations / threads;

    std::cout
        << "Testing "
        << threads
        << " thread"
        << (threads == 1 ? "" : "s")
        << " ("
        << operations_per_thread
        << " ops/thread, "
        << operations_per_thread * threads
        << " total)...\n";

    BenchmarkResult result =
        RunBenchmark(
            seed_db_file,
            seed_wal_file,
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

        << "  Failed: "
        << result.failed << "\n\n";
  }

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
      << std::setw(12)
      << "Failed"
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
        << result.tps

        << std::setw(12)
        << result.p50_ms

        << std::setw(12)
        << result.p95_ms

        << std::setw(12)
        << result.p99_ms

        << std::setw(12)
        << result.failed

        << "\n";
  }

  std::cout
      << "============================================================\n";

  return 0;
}