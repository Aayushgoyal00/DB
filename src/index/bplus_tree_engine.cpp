#include "index/bplus_tree_engine.h"

#include <algorithm>
#include <utility>

namespace dbengine {

BPlusTreeEngine::BPlusTreeEngine(DiskManager* disk_manager)
    : disk_manager_(disk_manager) {}

class BPlusTreeEngine::IteratorImpl final : public Iterator {
 public:
  IteratorImpl(const LeafNode* leaf, std::size_t index) : leaf_(leaf), index_(index) {
    SkipEmptyLeaves();
  }

  bool Valid() const override { return leaf_ != nullptr; }

  void Next() override {
    if (leaf_ == nullptr) {
      return;
    }
    ++index_;
    SkipEmptyLeaves();
  }

  const std::string& Key() const override { return leaf_->keys[index_]; }
  const std::string& Value() const override { return leaf_->values[index_]; }

 private:
  void SkipEmptyLeaves() {
    while (leaf_ != nullptr && index_ >= leaf_->keys.size()) {
      leaf_ = leaf_->next;
      index_ = 0;
    }
  }

  const LeafNode* leaf_;
  std::size_t index_;
};

Status BPlusTreeEngine::Get(const std::string& key, std::string* value_out) {
  if (value_out == nullptr) {
    return Status::InvalidArgument("value_out must not be null");
  }
  LeafNode* leaf = FindLeaf(key);
  if (leaf == nullptr) {
    return Status::NotFound("key not found");
  }

  const auto it = std::lower_bound(leaf->keys.begin(), leaf->keys.end(), key);
  if (it == leaf->keys.end() || *it != key) {
    return Status::NotFound("key not found");
  }
  *value_out = leaf->values[static_cast<std::size_t>(it - leaf->keys.begin())];
  return Status::OK();
}

Status BPlusTreeEngine::Put(const std::string& key, const std::string& value) {
  if (!root_) {
    auto leaf = std::make_unique<LeafNode>();
    leaf->keys.push_back(key);
    leaf->values.push_back(value);
    root_ = std::move(leaf);
    return Status::OK();
  }

  std::unique_ptr<Split> split = InsertRecursive(root_, key, value);
  if (split) {
    auto new_root = std::make_unique<InternalNode>();
    new_root->keys.push_back(std::move(split->separator));
    new_root->children.push_back(std::move(root_));
    new_root->children.push_back(std::move(split->right));
    root_ = std::move(new_root);
  }
  return Status::OK();
}

Status BPlusTreeEngine::Delete(const std::string& key) {
  LeafNode* leaf = FindLeaf(key);
  if (leaf == nullptr) {
    return Status::NotFound("key not found");
  }

  const auto it = std::lower_bound(leaf->keys.begin(), leaf->keys.end(), key);
  if (it == leaf->keys.end() || *it != key) {
    return Status::NotFound("key not found");
  }
  const std::size_t index = static_cast<std::size_t>(it - leaf->keys.begin());
  leaf->keys.erase(leaf->keys.begin() + static_cast<std::ptrdiff_t>(index));
  leaf->values.erase(leaf->values.begin() + static_cast<std::ptrdiff_t>(index));

  // Phase 0 deliberately leaves underfull nodes in place.  Phase 2 adds
  // redistribution/merging once the cost of an underfull on-disk page is real.
  return Status::OK();
}

std::unique_ptr<Iterator> BPlusTreeEngine::Scan(
    const std::string& start_key) {
  if (!root_) {
    return std::make_unique<IteratorImpl>(nullptr, 0);
  }
  LeafNode* leaf = FindLeaf(start_key);
  const auto it = std::lower_bound(leaf->keys.begin(), leaf->keys.end(), start_key);
  return std::make_unique<IteratorImpl>(leaf,
                                        static_cast<std::size_t>(it - leaf->keys.begin()));
}

BPlusTreeEngine::LeafNode* BPlusTreeEngine::FindLeaf(const std::string& key) const {
  Node* node = root_.get();
  while (node != nullptr && !node->is_leaf) {
    auto* internal = static_cast<InternalNode*>(node);
    const auto it = std::upper_bound(internal->keys.begin(), internal->keys.end(), key);
    node = internal->children[static_cast<std::size_t>(it - internal->keys.begin())].get();
  }
  return static_cast<LeafNode*>(node);
}

std::unique_ptr<BPlusTreeEngine::Split> BPlusTreeEngine::InsertRecursive(
    std::unique_ptr<Node>& node, const std::string& key, const std::string& value) {
  if (node->is_leaf) {
    auto* leaf = static_cast<LeafNode*>(node.get());
    const auto it = std::lower_bound(leaf->keys.begin(), leaf->keys.end(), key);
    const std::size_t index = static_cast<std::size_t>(it - leaf->keys.begin());
    if (it != leaf->keys.end() && *it == key) {
      leaf->values[index] = value;
      return nullptr;
    }
    leaf->keys.insert(it, key);
    leaf->values.insert(leaf->values.begin() + static_cast<std::ptrdiff_t>(index), value);
    return leaf->keys.size() > kMaxKeys ? SplitLeaf(leaf) : nullptr;
  }

  auto* internal = static_cast<InternalNode*>(node.get());
  const auto child_it = std::upper_bound(internal->keys.begin(), internal->keys.end(), key);
  const std::size_t child_index =
      static_cast<std::size_t>(child_it - internal->keys.begin());
  std::unique_ptr<Split> child_split =
      InsertRecursive(internal->children[child_index], key, value);
  if (!child_split) {
    return nullptr;
  }

  internal->keys.insert(internal->keys.begin() + static_cast<std::ptrdiff_t>(child_index),
                        std::move(child_split->separator));
  internal->children.insert(
      internal->children.begin() + static_cast<std::ptrdiff_t>(child_index + 1),
      std::move(child_split->right));
  return internal->children.size() > kMaxChildren ? SplitInternal(internal) : nullptr;
}

std::unique_ptr<BPlusTreeEngine::Split> BPlusTreeEngine::SplitLeaf(LeafNode* leaf) {
  auto right = std::make_unique<LeafNode>();
  const std::size_t split_at = leaf->keys.size() / 2;
  right->keys.assign(std::make_move_iterator(leaf->keys.begin() +
                                              static_cast<std::ptrdiff_t>(split_at)),
                     std::make_move_iterator(leaf->keys.end()));
  right->values.assign(std::make_move_iterator(leaf->values.begin() +
                                                static_cast<std::ptrdiff_t>(split_at)),
                       std::make_move_iterator(leaf->values.end()));
  leaf->keys.resize(split_at);
  leaf->values.resize(split_at);
  right->next = leaf->next;
  leaf->next = right.get();

  auto split = std::make_unique<Split>();
  split->separator = right->keys.front();
  split->right = std::move(right);
  return split;
}

std::unique_ptr<BPlusTreeEngine::Split> BPlusTreeEngine::SplitInternal(
    InternalNode* internal) {
  const std::size_t promote_index = internal->keys.size() / 2;
  auto right = std::make_unique<InternalNode>();
  std::string separator = std::move(internal->keys[promote_index]);
  right->keys.assign(
      std::make_move_iterator(internal->keys.begin() +
                              static_cast<std::ptrdiff_t>(promote_index + 1)),
      std::make_move_iterator(internal->keys.end()));
  right->children.assign(
      std::make_move_iterator(internal->children.begin() +
                              static_cast<std::ptrdiff_t>(promote_index + 1)),
      std::make_move_iterator(internal->children.end()));
  internal->keys.resize(promote_index);
  internal->children.resize(promote_index + 1);

  auto split = std::make_unique<Split>();
  split->separator = std::move(separator);
  split->right = std::move(right);
  return split;
}

std::size_t BPlusTreeEngine::Height() const {
  std::size_t height = 0;
  const Node* node = root_.get();
  while (node != nullptr) {
    ++height;
    node = node->is_leaf ? nullptr : static_cast<const InternalNode*>(node)->children.front().get();
  }
  return height;
}

bool BPlusTreeEngine::ValidateInvariants(std::string* error_out) const {
  if (error_out != nullptr) {
    error_out->clear();
  }
  if (!root_) {
    return true;
  }
  std::size_t leaf_depth = 0;
  const LeafNode* previous_leaf = nullptr;
  return ValidateNode(root_.get(), 1, &leaf_depth, nullptr, nullptr, error_out,
                      &previous_leaf);
}

bool BPlusTreeEngine::ValidateNode(const Node* node, std::size_t depth,
                                   std::size_t* leaf_depth,
                                   const std::string* lower_bound,
                                   const std::string* upper_bound,
                                   std::string* error_out,
                                   const LeafNode** previous_leaf) const {
  const auto fail = [error_out](const std::string& message) {
    if (error_out != nullptr) {
      *error_out = message;
    }
    return false;
  };
  if (node->is_leaf) {
    const auto* leaf = static_cast<const LeafNode*>(node);
    if (leaf->keys.size() != leaf->values.size() || leaf->keys.size() > kMaxKeys) {
      return fail("leaf key/value count or capacity is invalid");
    }
    if (*previous_leaf != nullptr && (*previous_leaf)->next != leaf) {
      return fail("leaf sibling pointers do not match tree order");
    }
    for (std::size_t i = 0; i < leaf->keys.size(); ++i) {
      if ((i > 0 && leaf->keys[i - 1] >= leaf->keys[i]) ||
          (lower_bound != nullptr && leaf->keys[i] < *lower_bound) ||
          (upper_bound != nullptr && leaf->keys[i] >= *upper_bound)) {
        return fail("leaf keys are outside sorted separator bounds");
      }
    }
    if (*leaf_depth == 0) {
      *leaf_depth = depth;
    } else if (*leaf_depth != depth) {
      return fail("leaves are not at a common depth");
    }
    *previous_leaf = leaf;
    return true;
  }

  const auto* internal = static_cast<const InternalNode*>(node);
  if (internal->keys.size() > kMaxKeys ||
      internal->children.size() != internal->keys.size() + 1) {
    return fail("internal key/child count or capacity is invalid");
  }
  for (std::size_t i = 1; i < internal->keys.size(); ++i) {
    if (internal->keys[i - 1] >= internal->keys[i]) {
      return fail("internal separator keys are not sorted");
    }
  }
  for (std::size_t i = 0; i < internal->children.size(); ++i) {
    const std::string* child_lower = i == 0 ? lower_bound : &internal->keys[i - 1];
    const std::string* child_upper = i == internal->keys.size() ? upper_bound : &internal->keys[i];
    if (!ValidateNode(internal->children[i].get(), depth + 1, leaf_depth,
                      child_lower, child_upper, error_out, previous_leaf)) {
      return false;
    }
  }
  return true;
}

} // namespace dbengine
