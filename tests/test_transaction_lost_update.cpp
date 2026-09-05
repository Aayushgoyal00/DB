#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
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

constexpr int kInitialValue = 0;
constexpr int kDefaultTotalOperations = 2000;
constexpr int kDefaultTimeoutSeconds = 60;
constexpr int kHeartbeatSeconds = 5;

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

enum class Variant {
  kSingleCounter,
  kMultipleCounters,
};

struct RunResult {
  Variant variant = Variant::kSingleCounter;
  int threads = 0;
  int total_operations = 0;
  int committed = 0;
  int failed = 0;
  int completed_operations = 0;
  double elapsed_seconds = 0.0;
  bool timed_out = false;
  bool lost_update_pass = false;
  bool invariant_pass = false;
  bool invariant_checked = false;
  int expected_final_total = 0;
  int actual_final_total = 0;
  std::string invariant_error;
};

struct RunState {
  std::unique_ptr<dbengine::DiskManager> disk;
  std::unique_ptr<dbengine::LogManager> wal;
  std::unique_ptr<dbengine::BufferPoolManager> bpm;
  std::unique_ptr<dbengine::BPlusTreeEngine> engine;

  std::atomic<int> committed{0};
  std::atomic<int> failed{0};
  std::atomic<int> completed_operations{0};
  std::atomic<int> completed_threads{0};
  std::atomic<bool> timed_out{false};
  std::mutex progress_mutex;
  std::condition_variable progress_cv;
};

std::string VariantName(Variant variant) {
  return variant == Variant::kSingleCounter
             ? "A-single-counter"
             : "B-multiple-counters";
}

std::vector<std::string> KeysForVariant(Variant variant) {
  if (variant == Variant::kSingleCounter) {
    return {"counter"};
  }

  std::vector<std::string> keys;
  for (int key_number = 0; key_number < 10; ++key_number) {
    keys.push_back("counter:" + std::to_string(key_number));
  }
  return keys;
}

bool ParseInteger(const std::string& text, int* value_out) {
  if (value_out == nullptr || text.empty()) {
    return false;
  }

  const char* begin = text.data();
  const char* end = begin + text.size();
  int value = 0;
  const auto parsed = std::from_chars(begin, end, value);
  if (parsed.ec != std::errc() || parsed.ptr != end) {
    return false;
  }

  *value_out = value;
  return true;
}

bool InitializeCounters(dbengine::BPlusTreeEngine* engine,
                        const std::vector<std::string>& keys) {
  for (const std::string& key : keys) {
    auto transaction = engine->BeginTransaction();
    if (!engine->Put(transaction.get(), key, std::to_string(kInitialValue)).ok()) {
      engine->Abort(transaction.get());
      return false;
    }
    if (!engine->Commit(transaction.get()).ok()) {
      engine->Abort(transaction.get());
      return false;
    }
  }
  return true;
}

RunResult RunOne(Variant variant,
                 int thread_count,
                 int total_operations,
                 int timeout_seconds,
                 const std::string& file_stem) {
  RunResult result;
  result.variant = variant;
  result.threads = thread_count;
  result.total_operations = total_operations;

  const int operations_per_thread = total_operations / thread_count;
  const std::vector<std::string> keys = KeysForVariant(variant);
  const std::string db_file = file_stem + ".db";
  const std::string wal_file = file_stem + ".wal";

  std::error_code remove_error;
  std::filesystem::remove(db_file, remove_error);
  std::filesystem::remove(wal_file, remove_error);

  auto state = std::make_shared<RunState>();
  state->disk = std::make_unique<dbengine::DiskManager>(db_file);
  state->wal = std::make_unique<dbengine::LogManager>(wal_file);
  state->bpm = std::make_unique<dbengine::BufferPoolManager>(64,
                                                               state->disk.get(),
                                                               state->wal.get());
  state->engine = std::make_unique<dbengine::BPlusTreeEngine>(state->bpm.get(),
                                                                state->wal.get(),
                                                                true);
  if (!state->engine->ValidateInvariants() ||
      !InitializeCounters(state->engine.get(), keys)) {
    result.failed = total_operations;
    result.invariant_error = "counter initialization failed";
    return result;
  }

  StartGate start_gate(thread_count);
  std::vector<std::thread> workers;
  workers.reserve(thread_count);
  const auto start = Clock::now();

  for (int thread_id = 0; thread_id < thread_count; ++thread_id) {
    workers.emplace_back([state, &start_gate, keys, thread_id,
                          operations_per_thread, variant]() {
      start_gate.Wait();

      for (int operation = 0; operation < operations_per_thread; ++operation) {
        const std::size_t key_index =
            variant == Variant::kSingleCounter
                ? 0
                : static_cast<std::size_t>((thread_id + operation) % keys.size());
        const std::string& key = keys[key_index];
        auto transaction = state->engine->BeginTransaction();
        std::string value;
        bool success = state->engine->Get(transaction.get(), key, &value).ok();
        int parsed_value = 0;
        success = success && ParseInteger(value, &parsed_value);

        if (success) {
          success = state->engine->Put(transaction.get(), key,
                                       std::to_string(parsed_value + 1)).ok();
        }

        if (!success) {
          state->engine->Abort(transaction.get());
          state->failed.fetch_add(1);
        } else if (!state->engine->Commit(transaction.get()).ok()) {
          state->engine->Abort(transaction.get());
          state->failed.fetch_add(1);
        } else {
          state->committed.fetch_add(1);
        }

        state->completed_operations.fetch_add(1);
        state->progress_cv.notify_all();
      }

      state->completed_threads.fetch_add(1);
      state->progress_cv.notify_all();
    });
  }

  bool all_threads_finished = false;
  {
    std::unique_lock<std::mutex> lock(state->progress_mutex);
    while (state->completed_threads.load() < thread_count) {
      if (state->progress_cv.wait_for(lock,
                                      std::chrono::seconds(kHeartbeatSeconds)) ==
          std::cv_status::timeout) {
        const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
        std::cout << "  heartbeat " << std::fixed << std::setprecision(1)
                  << elapsed << "s: completed "
                  << state->completed_operations.load() << "/"
                  << operations_per_thread * thread_count << " operations, "
                  << state->completed_threads.load() << "/" << thread_count
                  << " threads\n" << std::flush;
        if (elapsed >= timeout_seconds) {
          state->timed_out.store(true);
          break;
        }
      }
    }
    all_threads_finished = state->completed_threads.load() == thread_count;
  }

  if (all_threads_finished) {
    for (auto& worker : workers) {
      worker.join();
    }
  } else {
    for (auto& worker : workers) {
      worker.detach();
    }
    result.timed_out = true;
  }

  result.elapsed_seconds = std::chrono::duration<double>(Clock::now() - start).count();
  result.committed = state->committed.load();
  result.failed = state->failed.load();
  result.completed_operations = state->completed_operations.load();
  result.expected_final_total =
      kInitialValue * static_cast<int>(keys.size()) + result.committed;

  if (result.timed_out) {
    result.invariant_error = "worker threads did not finish before timeout";
    return result;
  }

  int final_sum = 0;
  bool values_valid = true;
  for (const std::string& key : keys) {
    std::string value;
    if (!state->engine->Get(nullptr, key, &value).ok()) {
      values_valid = false;
      break;
    }
    int parsed_value = 0;
    if (!ParseInteger(value, &parsed_value) || parsed_value < 0) {
      values_valid = false;
      break;
    }
    final_sum += parsed_value;
  }
  result.actual_final_total = final_sum;
  result.lost_update_pass = values_valid &&
                            final_sum == result.expected_final_total;

  result.invariant_pass = state->engine->ValidateInvariants(&result.invariant_error);
  result.invariant_checked = true;
  return result;
}

std::string LostUpdateVerdict(const RunResult& result) {
  if (result.timed_out) {
    return "SUSPECTED DEADLOCK/HANG";
  }
  if (!result.lost_update_pass) {
    return "LOST UPDATE DETECTED";
  }
  if (!result.invariant_pass) {
    return "INVARIANT VIOLATION";
  }
  return "PASS";
}

void PrintResult(const RunResult& result) {
  std::cout << VariantName(result.variant) << " / " << result.threads
            << " threads: " << LostUpdateVerdict(result)
            << " (elapsed " << std::fixed << std::setprecision(2)
            << result.elapsed_seconds << "s, committed " << result.committed
            << ", failed " << result.failed << ", completed "
            << result.completed_operations << "/" << result.total_operations << ")\n";
  if (!result.lost_update_pass && !result.timed_out) {
    std::cout << "  expected final total: " << result.expected_final_total
              << ", actual final total: " << result.actual_final_total << "\n";
  }
  if (result.invariant_checked && !result.invariant_pass &&
      !result.invariant_error.empty()) {
    std::cout << "  invariant error: " << result.invariant_error << "\n";
  }
}

}  // namespace

int main(int argc, char** argv) {
  const int total_operations = argc > 1 ? std::atoi(argv[1]) : kDefaultTotalOperations;
  const int timeout_seconds = argc > 2 ? std::atoi(argv[2]) : kDefaultTimeoutSeconds;

  if (total_operations <= 0 || timeout_seconds <= 0) {
    std::cerr << "Usage: test_transaction_lost_update [total_operations] [timeout_seconds]\n";
    return 1;
  }

  const std::vector<int> thread_counts = {1, 2, 4, 8, 16, 32};
  std::vector<RunResult> results;

  std::cout << "Lost-update correctness check\n"
            << "Total operations per configuration: " << total_operations << "\n"
            << "Timeout per configuration: " << timeout_seconds << "s\n"
            << "Heartbeat interval: " << kHeartbeatSeconds << "s\n\n";

  for (Variant variant : {Variant::kSingleCounter, Variant::kMultipleCounters}) {
    std::cout << "Variant " << VariantName(variant) << "\n";
    for (int thread_count : thread_counts) {
      std::cout << "Running " << thread_count << " threads...\n" << std::flush;
      RunResult result = RunOne(variant, thread_count, total_operations,
                                timeout_seconds,
                                "lost_update_" + VariantName(variant) + "_" +
                                    std::to_string(thread_count));
      PrintResult(result);
      results.push_back(result);
      if (result.timed_out) {
        std::cout << "  stopping this variant after suspected deadlock/hang\n";
        break;
      }
    }
    std::cout << "\n";
  }

  std::cout << "\n============================================================\n"
            << std::left << std::setw(22) << "Variant"
            << std::setw(9) << "Threads"
            << std::setw(24) << "Lost-update"
            << std::setw(18) << "Invariants"
            << std::setw(12) << "Elapsed(s)" << "\n"
            << "------------------------------------------------------------\n";
  for (const RunResult& result : results) {
    std::cout << std::left << std::setw(22) << VariantName(result.variant)
              << std::setw(9) << result.threads
              << std::setw(24) << (result.timed_out ? "SUSPECTED DEADLOCK/HANG"
                                                     : (result.lost_update_pass ? "PASS"
                                                                                 : "FAIL"))
                    << std::setw(18)
                    << (result.invariant_checked
                      ? (result.invariant_pass ? "PASS" : "FAIL")
                      : "NOT CHECKED")
              << std::setw(12) << std::fixed << std::setprecision(2)
              << result.elapsed_seconds << "\n";
  }
  std::cout << "============================================================\n";

  for (const RunResult& result : results) {
    if (result.timed_out || !result.lost_update_pass || !result.invariant_pass) {
      return 1;
    }
  }
  return 0;
}
