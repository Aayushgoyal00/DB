#include <algorithm>
#include <iomanip>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "index/bplus_tree_engine.h"

using dbengine::BPlusTreeEngine;
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
    BPlusTreeEngine tree(nullptr);
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
      const std::string string_key = Key(key);
      if (!tree.Put(string_key, "value-" + string_key).ok() ||
          !CheckInvariants(tree, &error)) {
        valid_after_every_insert = false;
        break;
      }
    }
    Check(valid_after_every_insert, std::string(order) + " inserts preserve invariants");
    Check(tree.Height() >= 3, std::string(order) + " inserts create a 3+ level tree");
  }
}

void TestRootSplitAndUpdates() {
  BPlusTreeEngine tree(nullptr);
  bool inserted = true;
  for (int i = 0; i < 80; ++i) {
    inserted = tree.Put(Key(i), "old").ok() && inserted;
  }
  Check(inserted, "insert keys for root-split test");
  Check(tree.Height() >= 3, "root split grows a second internal level");
  Check(tree.Put(Key(40), "new").ok(), "duplicate Put updates existing value");
  std::string value;
  Check(tree.Get(Key(40), &value).ok() && value == "new", "updated value is searchable");
  Check(tree.Get(Key(999), &value).IsNotFound(), "missing key reports NotFound");
  Check(tree.Get(Key(1), nullptr).code() == Status::Code::kInvalidArgument,
        "Get rejects a null output pointer");
}

void TestOracleWithDeletes() {
  BPlusTreeEngine tree(nullptr);
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
    if (operation % 100 == 0 && !MatchesOracle(&tree, oracle, &error)) {
      operations_succeeded = false;
      break;
    }
  }
  if (operations_succeeded) {
    operations_succeeded = MatchesOracle(&tree, oracle, &error);
  }
  Check(operations_succeeded, "15k random Put/Delete operations match std::map: " + error);
}

}  // namespace

int main() {
  TestInsertionOrders();
  TestRootSplitAndUpdates();
  TestOracleWithDeletes();

  if (g_failures == 0) {
    std::cout << "\nAll checks passed.\n";
    return 0;
  }
  std::cout << "\n" << g_failures << " check(s) failed.\n";
  return 1;
}
