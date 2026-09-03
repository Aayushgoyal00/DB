#include "txn/recovery_manager.h"

#include <limits>

namespace dbengine {

RecoveryManager::RecoveryManager(LogManager* log_manager, RecoveryTarget* target)
    : log_manager_(log_manager), target_(target) {}

Status RecoveryManager::Recover() {
  Status s = AnalysisPass();
  if (!s.ok()) return s;
  s = RedoPass();
  if (!s.ok()) return s;
  return UndoPass();
}

Status RecoveryManager::AnalysisPass() {
  // Walk the entire log once. For each record:
  //   - If it's Begin/Commit/Abort, add/remove the txn from active_txn_table_.
  //   - If it's Insert/Update/Delete, the txn is now active (insert if new),
  //     the page is dirty (set recLSN if first time we see it), and append
  //     to that txn's undo chain.
  return log_manager_->IterateAll([this](const LogRecord& r) {
    switch (r.type) {
      case LogRecordType::kBegin:
        active_txn_set_.insert(r.txn_id);
        active_txn_table_[r.txn_id] = r.lsn;
        break;
      case LogRecordType::kCommit:
        active_txn_set_.erase(r.txn_id);
        active_txn_table_.erase(r.txn_id);
        committed_txn_set_.insert(r.txn_id);
        break;
      case LogRecordType::kAbort:
        active_txn_set_.erase(r.txn_id);
        active_txn_table_.erase(r.txn_id);
        aborted_txn_set_.insert(r.txn_id);
        break;
      case LogRecordType::kInsert:
      case LogRecordType::kUpdate:
      case LogRecordType::kDelete:
        if (!active_txn_table_.count(r.txn_id) && !committed_txn_set_.count(r.txn_id) &&
            !aborted_txn_set_.count(r.txn_id)) {
          // Implicit txn start — happens if Begin was lost in a torn write.
          active_txn_set_.insert(r.txn_id);
        }
        active_txn_table_[r.txn_id] = r.lsn;
        if (r.page_id != INVALID_PAGE_ID &&
            !dirty_page_table_.count(r.page_id)) {
          dirty_page_table_[r.page_id] = r.lsn;
        }
        txn_chains_[r.txn_id].push_back(
            {r.lsn, r.page_id, r.key, r.before_image});
        break;
      case LogRecordType::kCheckpoint:
        // Phase 3: ignored. A real impl would truncate recovery work here.
        break;
    }
  });
}

Status RecoveryManager::RedoPass() {
  // Walk log forward; re-apply after-image for every record on a dirty
  // page whose recLSN <= record LSN. Phase 3 has no "skip if already
  // applied" optimization (would need a per-page last-applied-LSN) — we
  // trust that Redo's effect is idempotent enough at the engine layer
  // (it is, for our slotted-page model).
  if (dirty_page_table_.empty()) return Status::OK();  // nothing dirty

  lsn_t min_rec_lsn = std::numeric_limits<lsn_t>::max();
  for (const auto& [pid, rec] : dirty_page_table_) {
    if (rec < min_rec_lsn) min_rec_lsn = rec;
  }

  Status first = Status::OK();
  log_manager_->IterateAll([this, min_rec_lsn, &first](const LogRecord& r) {
    if (r.lsn < min_rec_lsn) return;  // before earliest dirty page
    if (r.type != LogRecordType::kInsert &&
        r.type != LogRecordType::kUpdate &&
        r.type != LogRecordType::kDelete) return;
    if (!dirty_page_table_.count(r.page_id)) return;

    Status s = target_->Redo(r.page_id, r.key, r.after_image);
    if (!s.ok() && first.ok()) first = s;
  });
  return first;
}

Status RecoveryManager::UndoPass() {
  // For every txn still active or aborted, walk its record chain backward applying
  // before-images. This is where uncommitted work gets rolled back.
  Status first = Status::OK();
  std::unordered_set<txn_id_t> txns_to_undo;
  for (txn_id_t tid : active_txn_set_) txns_to_undo.insert(tid);
  for (txn_id_t tid : aborted_txn_set_) txns_to_undo.insert(tid);

  for (txn_id_t txn_id : txns_to_undo) {
    auto it = txn_chains_.find(txn_id);
    if (it == txn_chains_.end()) continue;
    const auto& chain = it->second;
    for (auto rit = chain.rbegin(); rit != chain.rend(); ++rit) {
      Status s = target_->Undo(rit->page_id, rit->key, rit->before_image);
      if (!s.ok() && first.ok()) first = s;
    }
  }
  return first;
}

}  // namespace dbengine
