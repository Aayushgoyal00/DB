#pragma once

#include <string>
#include <unordered_set>
#include <vector>

#include "common/config.h"
#include "storage/log_record.h"

namespace dbengine {

enum class TransactionState {
  kGrowing,
  kShrinking,
  kCommitted,
  kAborted,
};

// Represents an in-flight transaction in the database engine. Tracks transaction
// state, locks held for two-phase locking (2PL), and undo history for rollback.
class Transaction {
 public:
  struct UndoRecord {
    page_id_t page_id = INVALID_PAGE_ID;
    std::string key;
    std::string before_image;
  };

  explicit Transaction(txn_id_t txn_id) : txn_id_(txn_id) {}
  ~Transaction() = default;

  Transaction(const Transaction&) = delete;
  Transaction& operator=(const Transaction&) = delete;

  txn_id_t GetTxnId() const { return txn_id_; }

  TransactionState GetState() const { return state_; }
  void SetState(TransactionState state) { state_ = state; }

  lsn_t GetPrevLSN() const { return prev_lsn_; }
  void SetPrevLSN(lsn_t lsn) { prev_lsn_ = lsn; }

  const std::unordered_set<std::string>& GetSharedLockSet() const {
    return shared_lock_set_;
  }
  const std::unordered_set<std::string>& GetExclusiveLockSet() const {
    return exclusive_lock_set_;
  }

  bool HoldsSharedLock(const std::string& key) const {
    return shared_lock_set_.count(key) > 0;
  }
  bool HoldsExclusiveLock(const std::string& key) const {
    return exclusive_lock_set_.count(key) > 0;
  }

  void AddSharedLock(const std::string& key) {
    shared_lock_set_.insert(key);
  }
  void AddExclusiveLock(const std::string& key) {
    exclusive_lock_set_.insert(key);
  }

  void RemoveSharedLock(const std::string& key) {
    shared_lock_set_.erase(key);
  }
  void RemoveExclusiveLock(const std::string& key) {
    exclusive_lock_set_.erase(key);
  }

  void ClearLocks() {
    shared_lock_set_.clear();
    exclusive_lock_set_.clear();
  }

  void AppendUndoRecord(page_id_t page_id, std::string key,
                        std::string before_image) {
    undo_records_.push_back({page_id, std::move(key), std::move(before_image)});
  }

  const std::vector<UndoRecord>& GetUndoRecords() const {
    return undo_records_;
  }

  void ClearUndoRecords() {
    undo_records_.clear();
  }

 private:
  txn_id_t txn_id_;
  TransactionState state_ = TransactionState::kGrowing;
  lsn_t prev_lsn_ = INVALID_LSN;

  std::unordered_set<std::string> shared_lock_set_;
  std::unordered_set<std::string> exclusive_lock_set_;
  std::vector<UndoRecord> undo_records_;
};

}  // namespace dbengine
