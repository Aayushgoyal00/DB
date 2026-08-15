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
#include "storage/disk_manager.h"

using dbengine::BPlusTreeEngine;
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

std::string StructuralKey(int value) {
  return Key(value) + std::string(600, 'k');
}

bool CheckInvariants(const BPlusTreeEngine& tree, std::string* error) {
  return tree.ValidateInvariants(error);
}

bool MatchesOracle(BPlusTreeEngine* tree, const std::map<std::string, std::string>& oracle,
                   std::string* error) {
  std::string invariant_error;
  if (!CheckInvariants(*tree, &invariant_error)) {
    *error = "invariants failed: " + invariant_error;
    return false;
  }

  auto iterator = tree->Scan("");
  for (const auto& [key, value] : oracle) {
    if (!iterator->Valid() || iterator->Key() != key || iterator->Value() != value) {
      *error = "scan differs from std::map at " + key;
      return false;
    }
    iterator->Next();
  }
  if (iterator->Valid()) {
    *error = "scan has keys absent from std::map";
    return false;
  }

  for (const auto& [key, value] : oracle) {
    std::string found;
    Status status = tree->Get(key, &found);
    if (!status.ok() || found != value) {
      *error = "Get differs from std::map at " + key;
      return false;
    }
  }
  return true;
}

void TestInsertionOrders() {
  for (const char* order : {"ascending", "descending", "random"}) {
    const std::string filename = std::string("test_bplus_") + order + ".db";
    std::remove(filename.c_str());
    {
    DiskManager disk_manager(filename);
    BPlusTreeEngine tree(&disk_manager);
    std::vector<int> keys(200);
    for (int i = 0; i < 200; ++i) {
      keys[static_cast<std::size_t>(i)] = i;
    }
    if (std::string(order) == "descending") {
      std::reverse(keys.begin(), keys.end());
    } else if (std::string(order) == "random") {
      std::mt19937 generator(42);
      std::shuffle(keys.begin(), keys.end(), generator);
    }

    bool valid_after_every_insert = true;
    std::string error;
    for (int key : keys) {
      const std::string string_key = StructuralKey(key);
      if (!tree.Put(string_key, "value").ok() ||
          !CheckInvariants(tree, &error)) {
        valid_after_every_insert = false;
        break;
      }
    }
    Check(valid_after_every_insert, std::string(order) + " inserts preserve invariants");
    Check(tree.Height() >= 3, std::string(order) + " inserts create a 3+ level tree");
    }
    std::remove(filename.c_str());
  }
}

void TestRootSplitAndUpdates() {
  const char* filename = "test_bplus_root_split.db";
  std::remove(filename);
  {
  DiskManager disk_manager(filename);
  BPlusTreeEngine tree(&disk_manager);
  bool inserted = true;
  for (int i = 0; i < 200; ++i) {
    inserted = tree.Put(StructuralKey(i), "old").ok() && inserted;
  }
  Check(inserted, "insert keys for root-split test");
  Check(tree.Height() >= 3, "root split grows a second internal level");
  Check(tree.Put(StructuralKey(40), "new").ok(), "duplicate Put updates existing value");
  std::string value;
  Check(tree.Get(StructuralKey(40), &value).ok() && value == "new", "updated value is searchable");
  Check(tree.Get(StructuralKey(999), &value).IsNotFound(), "missing key reports NotFound");
  Check(tree.Get(StructuralKey(1), nullptr).code() == Status::Code::kInvalidArgument,
        "Get rejects a null output pointer");
  }
  std::remove(filename);
}

void TestOracleWithDeletes() {
  const char* filename = "test_bplus_oracle.db";
  std::remove(filename);
  {
  DiskManager disk_manager(filename);
  BPlusTreeEngine tree(&disk_manager);
  std::map<std::string, std::string> oracle;
  std::mt19937 generator(20260814);
  std::uniform_int_distribution<int> key_distribution(0, 14999);
  std::bernoulli_distribution delete_distribution(0.35);

  // More than 10k randomized operations, checked periodically against an
  // independent ordered-map implementation.
  bool operations_succeeded = true;
  std::string error;
  for (int operation = 1; operation <= 15000; ++operation) {
    const std::string key = Key(key_distribution(generator));
    if (delete_distribution(generator)) {
      Status status = tree.Delete(key);
      const std::size_t removed = oracle.erase(key);
      if ((removed == 0 && !status.IsNotFound()) || (removed == 1 && !status.ok())) {
        operations_succeeded = false;
        error = "Delete status differs from oracle";
        break;
      }
    } else {
      const std::string value = "value-" + std::to_string(operation);
      if (!tree.Put(key, value).ok()) {
        operations_succeeded = false;
        error = "Put failed";
        break;
      }
      oracle[key] = value;
    }
    if (operation % 5000 == 0 && !MatchesOracle(&tree, oracle, &error)) {
      operations_succeeded = false;
      break;
    }
  }
  if (operations_succeeded) {
    operations_succeeded = MatchesOracle(&tree, oracle, &error);
  }
  Check(operations_succeeded, "15k random Put/Delete operations match std::map: " + error);
  }
  std::remove(filename);
}

void TestReopenAndScan() {
  const char* filename = "test_bplus_reopen.db";
  std::remove(filename);
  {
    DiskManager disk_manager(filename);
    BPlusTreeEngine tree(&disk_manager);
    bool inserted = true;
    for (int i = 0; i < 300; ++i) {
      inserted = tree.Put(Key(i), std::string(500, 'p') + std::to_string(i)).ok() && inserted;
    }
    Check(inserted, "insert persistent keys");
  }
  {
    DiskManager disk_manager(filename);
    BPlusTreeEngine tree(&disk_manager);
    std::string value;
    Check(tree.Get(Key(173), &value).ok() && value == std::string(500, 'p') + "173",
          "tree survives close and reopen");
    auto iterator = tree.Scan(Key(150));
    bool sorted_range = true;
    for (int i = 150; i < 300; ++i) {
      sorted_range = iterator->Valid() && iterator->Key() == Key(i) && sorted_range;
      iterator->Next();
    }
    Check(sorted_range && !iterator->Valid(),
          "Scan returns a sorted range across persistent leaf siblings");
  }
  std::remove(filename);
}

}  // namespace

int main() {
  TestInsertionOrders();
  TestRootSplitAndUpdates();
  TestOracleWithDeletes();
  TestReopenAndScan();

  if (g_failures == 0) {
    std::cout << "\nAll checks passed.\n";
    return 0;
  }
  std::cout << "\n" << g_failures << " check(s) failed.\n";
  return 1;
}
