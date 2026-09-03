#include "txn/lock_manager.h"

#include <algorithm>

namespace dbengine {

std::shared_ptr<LockManager::LockHead> LockManager::GetOrCreateLockHead(
    const std::string& key) {
  std::lock_guard<std::mutex> lock(global_latch_);
  auto it = lock_table_.find(key);
  if (it != lock_table_.end()) {
    return it->second;
  }
  auto head = std::make_shared<LockHead>();
  lock_table_[key] = head;
  return head;
}

Status LockManager::AcquireShared(Transaction* txn, const std::string& key) {
  if (txn == nullptr) {
    return Status::InvalidArgument("txn must not be null");
  }
  if (txn->GetState() == TransactionState::kShrinking) {
    return Status::InvalidArgument("2PL violation: cannot acquire lock in shrinking phase");
  }
  if (txn->GetState() != TransactionState::kGrowing) {
    return Status::InvalidArgument("transaction is not active");
  }
  if (txn->HoldsSharedLock(key) || txn->HoldsExclusiveLock(key)) {
    return Status::OK();
  }

  auto head = GetOrCreateLockHead(key);
  std::unique_lock<std::mutex> lock(head->mutex);

  head->request_queue.push_back({txn->GetTxnId(), LockMode::kShared, false});
  auto req_it = std::prev(head->request_queue.end());

  head->cv.wait(lock, [&]() {
    // Shared lock can be granted if not currently written, and all earlier
    // requests in queue before this one are either granted shared or also shared requests.
    if (head->is_writing) return false;
    for (auto it = head->request_queue.begin(); it != req_it; ++it) {
      if (it->lock_mode == LockMode::kExclusive) return false;
    }
    return true;
  });

  req_it->granted = true;
  head->shared_count++;
  head->shared_txn_ids.insert(txn->GetTxnId());
  txn->AddSharedLock(key);

  return Status::OK();
}

Status LockManager::AcquireExclusive(Transaction* txn, const std::string& key) {
  if (txn == nullptr) {
    return Status::InvalidArgument("txn must not be null");
  }
  if (txn->GetState() == TransactionState::kShrinking) {
    return Status::InvalidArgument("2PL violation: cannot acquire lock in shrinking phase");
  }
  if (txn->GetState() != TransactionState::kGrowing) {
    return Status::InvalidArgument("transaction is not active");
  }
  if (txn->HoldsExclusiveLock(key)) {
    return Status::OK();
  }

  auto head = GetOrCreateLockHead(key);
  std::unique_lock<std::mutex> lock(head->mutex);

  bool upgrading = txn->HoldsSharedLock(key);
  std::list<LockRequest>::iterator req_it;

  if (upgrading) {
    // Find and modify the existing shared request
    for (auto it = head->request_queue.begin(); it != head->request_queue.end(); ++it) {
      if (it->txn_id == txn->GetTxnId() && it->lock_mode == LockMode::kShared) {
        it->lock_mode = LockMode::kExclusive;
        req_it = it;
        break;
      }
    }
  } else {
    head->request_queue.push_back({txn->GetTxnId(), LockMode::kExclusive, false});
    req_it = std::prev(head->request_queue.end());
  }

  head->cv.wait(lock, [&]() {
    if (upgrading) {
      // Must be the only shared holder and no writer.
      return !head->is_writing && head->shared_count == 1 &&
             head->shared_txn_ids.count(txn->GetTxnId()) == 1;
    }
    // Exclusive can be granted only if first in queue, not writing, and no readers.
    return !head->is_writing && head->shared_count == 0 &&
           head->request_queue.begin() == req_it;
  });

  if (upgrading) {
    head->shared_count--;
    head->shared_txn_ids.erase(txn->GetTxnId());
    txn->RemoveSharedLock(key);
  }

  req_it->granted = true;
  head->is_writing = true;
  head->writing_txn_id = txn->GetTxnId();
  txn->AddExclusiveLock(key);

  return Status::OK();
}

Status LockManager::Unlock(Transaction* txn, const std::string& key) {
  if (txn == nullptr) {
    return Status::InvalidArgument("txn must not be null");
  }

  std::shared_ptr<LockHead> head;
  {
    std::lock_guard<std::mutex> lock(global_latch_);
    auto it = lock_table_.find(key);
    if (it == lock_table_.end()) {
      return Status::NotFound("lock not found for key");
    }
    head = it->second;
  }

  std::unique_lock<std::mutex> lock(head->mutex);

  bool holds_shared = txn->HoldsSharedLock(key);
  bool holds_exclusive = txn->HoldsExclusiveLock(key);

  if (!holds_shared && !holds_exclusive) {
    return Status::NotFound("transaction does not hold lock on key");
  }

  if (holds_shared) {
    head->shared_count--;
    head->shared_txn_ids.erase(txn->GetTxnId());
    txn->RemoveSharedLock(key);
  } else if (holds_exclusive) {
    head->is_writing = false;
    head->writing_txn_id = INVALID_TXN_ID;
    txn->RemoveExclusiveLock(key);
  }

  // Remove request from queue
  for (auto it = head->request_queue.begin(); it != head->request_queue.end(); ++it) {
    if (it->txn_id == txn->GetTxnId()) {
      head->request_queue.erase(it);
      break;
    }
  }

  if (txn->GetState() == TransactionState::kGrowing) {
    txn->SetState(TransactionState::kShrinking);
  }

  head->cv.notify_all();
  return Status::OK();
}

Status LockManager::ReleaseAll(Transaction* txn) {
  if (txn == nullptr) {
    return Status::InvalidArgument("txn must not be null");
  }

  std::unordered_set<std::string> keys_to_unlock;
  for (const auto& k : txn->GetSharedLockSet()) {
    keys_to_unlock.insert(k);
  }
  for (const auto& k : txn->GetExclusiveLockSet()) {
    keys_to_unlock.insert(k);
  }

  for (const auto& key : keys_to_unlock) {
    std::shared_ptr<LockHead> head;
    {
      std::lock_guard<std::mutex> lock(global_latch_);
      auto it = lock_table_.find(key);
      if (it != lock_table_.end()) {
        head = it->second;
      }
    }
    if (head) {
      std::unique_lock<std::mutex> lock(head->mutex);
      if (txn->HoldsSharedLock(key)) {
        head->shared_count--;
        head->shared_txn_ids.erase(txn->GetTxnId());
      }
      if (txn->HoldsExclusiveLock(key)) {
        head->is_writing = false;
        head->writing_txn_id = INVALID_TXN_ID;
      }
      for (auto it = head->request_queue.begin(); it != head->request_queue.end(); ++it) {
        if (it->txn_id == txn->GetTxnId()) {
          head->request_queue.erase(it);
          break;
        }
      }
      head->cv.notify_all();
    }
  }

  txn->ClearLocks();
  return Status::OK();
}

}  // namespace dbengine
