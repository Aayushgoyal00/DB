#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/config.h"
#include "common/status.h"
#include "storage/log_manager.h"
#include "storage/log_record.h"

namespace dbengine {

// Minimal interface RecoveryManager needs from the engine layer. The
// default B+Tree engine implements this; tests can supply a mock. Keeping
// it narrow lets recovery be tested without dragging the whole tree in.
class RecoveryTarget {
 public:
  virtual ~RecoveryTarget() = default;

  // Re-apply an after-image for a key on a page. Used during REDO.
  virtual Status Redo(page_id_t page_id, const std::string& key,
                      const std::string& after_image) = 0;

  // Undo a before-image for a key on a page. Used during UNDO.
  virtual Status Undo(page_id_t page_id, const std::string& key,
                      const std::string& before_image) = 0;
};

// ARIES-lite recovery. Three passes, run on startup if the last shutdown
// wasn't clean:
//
//   1. ANALYSIS: scan log forward from offset 0, build:
//        - dirty_page_table_  (page_id -> recLSN)
//        - active_txn_table_  (txn_id   -> last LSN seen)
//      Plus a per-txn chain of log records, so UNDO can walk backward
//      without re-scanning the file.
//
//   2. REDO: replay every record's after-image, in LSN order, on pages
//      that were dirty. Brings the DB to the exact state at crash time
//      (including uncommitted work).
//
//   3. UNDO: for each txn still in active_txn_table_, walk its record
//      chain backward applying before-images. Rolls back uncommitted work.
//
// This implementation uses full before/after images, not physiological
// logging, for clarity. The trade-off is more log bytes per update.
class RecoveryManager {
 public:
  RecoveryManager(LogManager* log_manager, RecoveryTarget* target);

  // Run all three passes. Returns the first error encountered, or OK.
  Status Recover();

  // For tests / diagnostics.
  const std::unordered_map<page_id_t, lsn_t>& DirtyPageTable() const {
    return dirty_page_table_;
  }
  const std::unordered_set<txn_id_t>& ActiveTxns() const {
    return active_txn_set_;
  }

 private:
  Status AnalysisPass();
  Status RedoPass();
  Status UndoPass();

  LogManager* log_manager_;
  RecoveryTarget* target_;

  // Populated by Analysis, consumed by Redo/Undo.
  std::unordered_map<page_id_t, lsn_t> dirty_page_table_;
  std::unordered_map<txn_id_t, lsn_t> active_txn_table_;
  std::unordered_set<txn_id_t> active_txn_set_;
  std::unordered_set<txn_id_t> committed_txn_set_;
  std::unordered_set<txn_id_t> aborted_txn_set_;

  // Per-txn record chain (newest last), built in Analysis so UNDO can
  // walk backward without re-scanning.
  struct UndoEntry {
    lsn_t lsn;
    page_id_t page_id;
    std::string key;
    std::string before_image;
  };
  std::unordered_map<txn_id_t, std::vector<UndoEntry>> txn_chains_;
};

}  // namespace dbengine
