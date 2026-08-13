#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "index/kv_store.h"
#include "storage/disk_manager.h"

namespace dbengine {

// An in-memory B+Tree used to establish the tree algorithms before nodes are
// made page-backed in Phase 2.  The DiskManager argument is intentionally not
// used in this phase.
class BPlusTreeEngine : public KVStore {
 public:
  explicit BPlusTreeEngine(DiskManager* disk_manager);

  Status Get(const std::string& key, std::string* value_out) override;
  Status Put(const std::string& key, const std::string& value) override;
  Status Delete(const std::string& key) override;
  std::unique_ptr<Iterator> Scan(const std::string& start_key) override;

  // Diagnostics for tests and debug builds. They verify the properties that
  // make searches correct without exposing the node representation.
  bool ValidateInvariants(std::string* error_out = nullptr) const;
  std::size_t Height() const;

 private:
  static constexpr std::size_t kMaxChildren = 4;
  static constexpr std::size_t kMaxKeys = kMaxChildren - 1;

  struct Node {
    explicit Node(bool leaf) : is_leaf(leaf) {}
    virtual ~Node() = default;

    bool is_leaf;
  };

  struct LeafNode final : Node {
    LeafNode() : Node(true) {}

    std::vector<std::string> keys;
    std::vector<std::string> values;
    LeafNode* next = nullptr;
  };

  struct InternalNode final : Node {
    InternalNode() : Node(false) {}

    std::vector<std::string> keys;
    std::vector<std::unique_ptr<Node>> children;
  };

  struct Split {
    std::string separator;
    std::unique_ptr<Node> right;
  };

  class IteratorImpl;

  LeafNode* FindLeaf(const std::string& key) const;
  std::unique_ptr<Split> InsertRecursive(std::unique_ptr<Node>& node,
                                         const std::string& key,
                                         const std::string& value);
  std::unique_ptr<Split> SplitLeaf(LeafNode* leaf);
  std::unique_ptr<Split> SplitInternal(InternalNode* internal);
  bool ValidateNode(const Node* node, std::size_t depth,
                    std::size_t* leaf_depth, const std::string* lower_bound,
                    const std::string* upper_bound, std::string* error_out,
                    const LeafNode** previous_leaf) const;

  DiskManager* disk_manager_; // not owned
  page_id_t root_page_id_ = INVALID_PAGE_ID;
  std::unique_ptr<Node> root_;
};

} // namespace dbengine
