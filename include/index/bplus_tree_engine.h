#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "index/kv_store.h"
#include "storage/disk_manager.h"

namespace dbengine {

// Page-backed B+Tree. Page 0 is a small metadata page holding the root page
// id; every other node is serialized through SlottedPage.
class BPlusTreeEngine : public KVStore {
 public:
  explicit BPlusTreeEngine(DiskManager* disk_manager);

  Status Get(const std::string& key, std::string* value_out) override;
  Status Put(const std::string& key, const std::string& value) override;
  Status Delete(const std::string& key) override;
  std::unique_ptr<Iterator> Scan(const std::string& start_key) override;

  // Diagnostics retained for the Phase 0 property tests.
  bool ValidateInvariants(std::string* error_out = nullptr) const;
  std::size_t Height() const;

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

  Status ReadLeaf(page_id_t page_id, std::vector<LeafEntry>* entries,
                  page_id_t* right_sibling_page_id) const;
  Status WriteLeaf(page_id_t page_id, const std::vector<LeafEntry>& entries,
                   page_id_t right_sibling_page_id);
  Status ReadInternal(page_id_t page_id, InternalNodeData* node) const;
  Status WriteInternal(page_id_t page_id, const InternalNodeData& node);

  Status InsertRecursive(page_id_t page_id, const std::string& key,
                         const std::string& value,
                         std::optional<Split>* split_out);
  bool ValidatePage(page_id_t page_id, std::size_t depth,
                    std::size_t* leaf_depth, std::string* last_key,
                    page_id_t* expected_leaf_page_id,
                    std::string* error_out) const;

  DiskManager* disk_manager_;  // not owned
  page_id_t root_page_id_ = INVALID_PAGE_ID;
  Status initialization_status_;
};

}  // namespace dbengine
