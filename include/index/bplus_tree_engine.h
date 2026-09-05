#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

#include "index/kv_store.h"
#include "storage/buffer_pool_manager.h"
#include "storage/disk_manager.h"
#include "storage/log_manager.h"
#include "storage/slotted_page.h"
#include "txn/lock_manager.h"
#include "txn/recovery_manager.h"
#include "txn/transaction.h"

namespace dbengine {

// Page-backed B+Tree. Page 0 is a metadata page holding the root page id;
// every tree node is serialized as a slotted page.
//
// Construction modes:
//   (1) Direct-disk mode (Phase 0-2): BPlusTreeEngine(disk_manager)
//   (2) Buffer-pool mode (Phase 3+): BPlusTreeEngine(bpm, log_manager, recover_on_open)
class BPlusTreeEngine : public KVStore, public RecoveryTarget {
 public:
  // Direct-disk mode. All I/O hits DiskManager directly without caching or WAL.
  explicit BPlusTreeEngine(DiskManager* disk_manager);

  // Buffer-pool mode. All page I/O is managed by BufferPoolManager. If log_manager
  // is provided, updates are logged to WAL. If recover_on_open is true and log exists,
  // ARIES-lite recovery is executed on initialization.
  BPlusTreeEngine(BufferPoolManager* bpm, LogManager* log_manager = nullptr,
                  bool recover_on_open = true);

  ~BPlusTreeEngine() override;

  // KVStore interface implementation
  Status Get(const std::string& key, std::string* value_out) override;
  Status Put(const std::string& key, const std::string& value) override;
  Status Delete(const std::string& key) override;
  std::unique_ptr<Iterator> Scan(const std::string& start_key) override;

  // Transactional operations with Two-Phase Locking (2PL) and WAL
  std::unique_ptr<Transaction> BeginTransaction();
  Status Get(Transaction* txn, const std::string& key, std::string* value_out);
  Status Put(Transaction* txn, const std::string& key, const std::string& value);
  Status Delete(Transaction* txn, const std::string& key);
  Status Commit(Transaction* txn);
  Status Abort(Transaction* txn);

  // Diagnostics and invariant validation
  bool ValidateInvariants(std::string* error_out = nullptr) const;
  std::size_t Height() const;

  // RecoveryTarget implementation for ARIES-lite REDO and UNDO
  Status Redo(page_id_t page_id, const std::string& key,
              const std::string& after_image) override;
  Status Undo(page_id_t page_id, const std::string& key,
              const std::string& before_image) override;

  // Run ARIES-lite crash recovery
  Status Recover();

  // Write a clean shutdown marker
  Status MarkCleanShutdown();

  bool IsBufferPoolBacked() const { return bpm_ != nullptr; }
  LockManager* GetLockManager() { return &lock_manager_; }

 private:
  struct Split {
    std::string separator;
    page_id_t right_page_id;
  };

  struct LeafEntry {
    std::string key;
    std::string value;
  };

  struct InternalNodeData {
    std::vector<std::string> keys;
    std::vector<page_id_t> children;
  };

  class IteratorImpl;

  Status InitializeOrLoadMetadata();
  Status StoreRootPageId(page_id_t root_page_id);
  Status FindLeaf(const std::string& key, page_id_t* leaf_page_id) const;
  Status GetNodeType(page_id_t page_id, PageType* type_out) const;
  Status AllocateNewPage(page_id_t* page_id_out);

  Status ReadLeaf(page_id_t page_id, std::vector<LeafEntry>* entries,
                  page_id_t* right_sibling_page_id) const;
  Status WriteLeaf(page_id_t page_id, const std::vector<LeafEntry>& entries,
                   page_id_t right_sibling_page_id);
  Status ReadInternal(page_id_t page_id, InternalNodeData* node) const;
  Status WriteInternal(page_id_t page_id, const InternalNodeData& node);

  Status InsertRecursive(page_id_t page_id, const std::string& key,
                         const std::string& value,
                         std::optional<Split>* split_out);

  Status PutUnlogged(const std::string& key, const std::string& value);
  Status DeleteUnlogged(const std::string& key);

  bool ValidatePage(page_id_t page_id, std::size_t depth,
                    std::size_t* leaf_depth, std::string* last_key,
                    page_id_t* expected_leaf_page_id,
                    std::string* error_out) const;

  DiskManager* disk_manager_ = nullptr;
  BufferPoolManager* bpm_ = nullptr;
  LogManager* log_manager_ = nullptr;
  LockManager lock_manager_;
  std::mutex mutation_latch_;

  page_id_t root_page_id_ = INVALID_PAGE_ID;
  Status initialization_status_;

  std::atomic<lsn_t> next_txn_lsn_{1};
  std::atomic<txn_id_t> next_txn_id_{1};
  bool recovered_ = false;
};

}  // namespace dbengine
