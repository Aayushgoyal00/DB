#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>

#include "index/bplus_tree_engine.h"
#include "storage/buffer_pool_manager.h"
#include "storage/disk_manager.h"
#include "storage/log_manager.h"

int main(int argc, char** argv) {
  std::string db_file = "demo_db.db";
  std::string log_file = "demo_db.log";
  int count = 200;

  if (argc > 1) {
    db_file = argv[1];
  }
  if (argc > 2) {
    log_file = argv[2];
  }
  if (argc > 3) {
    count = std::atoi(argv[3]);
  }

  if (count <= 0) {
    std::cerr << "count must be > 0\n";
    return 1;
  }

  std::cout << "Creating DB: " << db_file << "\n";
  std::cout << "Creating WAL: " << log_file << "\n";
  std::cout << "Inserting " << count << " random key/value pairs\n";

  dbengine::DiskManager disk(db_file);
  dbengine::LogManager wal(log_file);
  dbengine::BufferPoolManager bpm(64, &disk, &wal);
  dbengine::BPlusTreeEngine engine(&bpm, &wal, true);

  std::mt19937_64 rng(123456789ULL);
  std::uniform_int_distribution<int> key_dist(1, 10000);
  std::uniform_int_distribution<int> value_dist(0, 999999);

  for (int i = 0; i < count; ++i) {
    const int key_val = key_dist(rng);
    const int value_val = value_dist(rng);

    const std::string key = "user:" + std::to_string(key_val);
    const std::string value = std::to_string(value_val);

    const auto status = engine.Put(key, value);
    if (!status.ok()) {
      std::cerr << "Put failed for key " << key << ": " << status.message() << "\n";
      return 1;
    }

    if (i % 25 == 0) {
      std::string out;
      const auto read_status = engine.Get(key, &out);
      if (!read_status.ok()) {
        std::cerr << "Get failed for key " << key << ": " << read_status.message() << "\n";
        return 1;
      }
    }
  }

  std::string result;
  const auto lookup = engine.Get("user:42", &result);
  if (lookup.ok()) {
    std::cout << "Sample lookup: user:42 => " << result << "\n";
  } else if (lookup.IsNotFound()) {
    std::cout << "Sample lookup: user:42 not found\n";
  } else {
    std::cout << "Sample lookup error: " << lookup.message() << "\n";
  }

  const auto clean = engine.MarkCleanShutdown();
  if (!clean.ok()) {
    std::cerr << "MarkCleanShutdown failed: " << clean.message() << "\n";
    return 1;
  }

  std::cout << "Random KV benchmark complete.\n";
  std::cout << "Files left on disk for inspection: " << db_file << " and " << log_file << "\n";
  return 0;
}
