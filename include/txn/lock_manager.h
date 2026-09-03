#pragma once

#include <condition_variable>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "common/config.h"
#include "common/status.h"
#include "txn/transaction.h"

namespace dbengine {

enum class LockMode {
  kShared,
  kExclusive,
};

// Key-level Lock Manager enforcing Two-Phase Locking (2PL) across transactions.
// Supports shared (read) and exclusive (write) locks with request queueing.
class LockManager {
 public:
  LockManager() = default;
  ~LockManager() = default;

  LockManager(const LockManager&) = delete;
  LockManager& operator=(const LockManager&) = delete;

  // Acquire a shared (read) lock on `key` for `txn`. Blocks until granted.
  Status AcquireShared(Transaction* txn, const std::string& key);

  // Acquire an exclusive (write) lock on `key` for `txn`. Blocks until granted.
  Status AcquireExclusive(Transaction* txn, const std::string& key);

  // Release a lock on `key` held by `txn`. Moves txn to shrinking phase if growing.
  Status Unlock(Transaction* txn, const std::string& key);

  // Release all locks held by `txn` upon commit or abort.
  Status ReleaseAll(Transaction* txn);

 private:
  struct LockRequest {
    txn_id_t txn_id;
    LockMode lock_mode;
    bool granted = false;
  };

  struct LockHead {
    std::mutex mutex;
    std::condition_variable cv;
    std::list<LockRequest> request_queue;
    bool is_writing = false;
    int shared_count = 0;
    txn_id_t writing_txn_id = INVALID_TXN_ID;
    std::unordered_set<txn_id_t> shared_txn_ids;
  };

  std::shared_ptr<LockHead> GetOrCreateLockHead(const std::string& key);

  std::mutex global_latch_;
  std::unordered_map<std::string, std::shared_ptr<LockHead>> lock_table_;
};

}  // namespace dbengine
