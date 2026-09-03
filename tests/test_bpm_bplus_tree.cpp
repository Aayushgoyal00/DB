#include <algorithm>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "index/bplus_tree_engine.h"
#include "storage/buffer_pool_manager.h"
#include "storage/disk_manager.h"

using dbengine::BPlusTreeEngine;
using dbengine::BufferPoolManager;
using dbengine::DiskManager;
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

std::string Key(int value) {
  std::ostringstream out;
  out << "key-" << std::setw(6) << std::setfill('0') << value;
  return out.str();
}

std::string LargeKey(int value) {
  return Key(value) + std::string(500, 'x');
}

void TestBpmTreeInsertionsAndGrowth() {
  const char* db_file = "test_bpm_tree_growth.db";
  std::remove(db_file);
  {
    DiskManager disk_manager(db_file);
    // Buffer pool with 10 frames
    BufferPoolManager bpm(10, &disk_manager);
    BPlusTreeEngine tree(&bpm);

    Check(tree.IsBufferPoolBacked(), "engine reports buffer-pool backed");

    // Insert 150 large keys to force multiple leaf splits and root splits
    bool inserted_all = true;
    for (int i = 0; i < 150; ++i) {
      Status s = tree.Put(LargeKey(i), "value-" + std::to_string(i));
      if (!s.ok()) {
        inserted_all = false;
        std::cerr << "Put failed at " << i << ": " << s.message() << "\n";
        break;
      }
    }
    Check(inserted_all, "inserted 150 keys into BPM-backed B+Tree");

    std::string error;
    Check(tree.ValidateInvariants(&error), "BPM-backed tree invariants valid after growth: " + error);
    Check(tree.Height() >= 3, "BPM-backed tree grew to 3+ levels");

    // Verify all keys are readable
    bool read_all = true;
    for (int i = 0; i < 150; ++i) {
      std::string val;
      Status s = tree.Get(LargeKey(i), &val);
      if (!s.ok() || val != "value-" + std::to_string(i)) {
        read_all = false;
        break;
      }
    }
    Check(read_all, "all 150 keys retrieved correctly from BPM tree");
  }

  // Close and reopen with fresh BPM
  {
    DiskManager disk_manager(db_file);
    BufferPoolManager bpm(10, &disk_manager);
    BPlusTreeEngine tree(&bpm);

    std::string error;
    Check(tree.ValidateInvariants(&error), "tree survives BPM close and reopen");

    // Scan all keys in sorted order
    auto it = tree.Scan("");
    int count = 0;
    while (it->Valid()) {
      ++count;
      it->Next();
    }
    Check(count == 150, "scanned exactly 150 keys across leaf siblings on reopen");
  }
  std::remove(db_file);
}

void TestBpmTreeOracleRandom() {
  const char* db_file = "test_bpm_tree_oracle.db";
  std::remove(db_file);
  {
    DiskManager disk_manager(db_file);
    BufferPoolManager bpm(15, &disk_manager);
    BPlusTreeEngine tree(&bpm);

    std::map<std::string, std::string> oracle;
    std::mt19937 gen(12345);
    std::uniform_int_distribution<int> key_dist(0, 499);
    std::bernoulli_distribution op_dist(0.35); // 35% deletes, 65% puts

    bool success = true;
    for (int i = 0; i < 2000; ++i) {
      std::string k = Key(key_dist(gen));
      if (op_dist(gen)) {
        Status s = tree.Delete(k);
        size_t erased = oracle.erase(k);
        if ((erased == 0 && !s.IsNotFound()) || (erased == 1 && !s.ok())) {
          success = false;
          std::cerr << "Oracle delete mismatch at step " << i << "\n";
          break;
        }
      } else {
        std::string v = "val-" + std::to_string(i);
        Status s = tree.Put(k, v);
        if (!s.ok()) {
          success = false;
          std::cerr << "Oracle put failed at step " << i << ": " << s.message() << "\n";
          break;
        }
        oracle[k] = v;
      }
    }

    Check(success, "2000 random operations against BPM tree");

    // Check all keys against oracle
    for (const auto& [k, v] : oracle) {
      std::string val;
      Status s = tree.Get(k, &val);
      if (!s.ok() || val != v) {
        success = false;
        break;
      }
    }
    Check(success, "BPM tree state matches std::map oracle exactly");

    std::string error;
    Check(tree.ValidateInvariants(&error), "invariants valid after 2000 random operations: " + error);
  }
  std::remove(db_file);
}

}  // namespace

int main() {
  TestBpmTreeInsertionsAndGrowth();
  TestBpmTreeOracleRandom();

  if (g_failures == 0) {
    std::cout << "\nAll BPM B+Tree checks passed.\n";
    return 0;
  }
  std::cout << "\n" << g_failures << " check(s) failed.\n";
  return 1;
}
