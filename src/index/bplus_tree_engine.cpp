#include "index/bplus_tree_engine.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>

#include "storage/page.h"
#include "storage/slotted_page.h"

namespace dbengine {
namespace {

constexpr std::array<char, 4> kMetadataMagic{'B', 'P', 'T', '1'};

uint32_t ReadU32(const char* data) {
  return static_cast<uint32_t>(static_cast<uint8_t>(data[0])) |
         (static_cast<uint32_t>(static_cast<uint8_t>(data[1])) << 8) |
         (static_cast<uint32_t>(static_cast<uint8_t>(data[2])) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(data[3])) << 24);
}

void WriteU32(char* data, uint32_t value) {
  data[0] = static_cast<char>(value & 0xffU);
  data[1] = static_cast<char>((value >> 8) & 0xffU);
  data[2] = static_cast<char>((value >> 16) & 0xffU);
  data[3] = static_cast<char>((value >> 24) & 0xffU);
}

Status PageFull(const Status& status) {
  return status.ok() ? Status::OK() : Status::InvalidArgument("node does not fit in a page");
}

}  // namespace

class BPlusTreeEngine::IteratorImpl final : public Iterator {
 public:
  IteratorImpl(const BPlusTreeEngine* tree, page_id_t page_id, std::size_t index)
      : tree_(tree), page_id_(page_id), index_(index) {
    LoadPage();
  }

  bool Valid() const override { return valid_; }

  void Next() override {
    if (!valid_) {
      return;
    }
    ++index_;
    while (valid_ && index_ >= entries_.size()) {
      if (right_sibling_page_id_ == INVALID_PAGE_ID) {
        valid_ = false;
        return;
      }
      page_id_ = right_sibling_page_id_;
      index_ = 0;
      LoadPage();
    }
  }

  const std::string& Key() const override { return entries_[index_].key; }
  const std::string& Value() const override { return entries_[index_].value; }

 private:
  void LoadPage() {
    entries_.clear();
    right_sibling_page_id_ = INVALID_PAGE_ID;
    valid_ = page_id_ != INVALID_PAGE_ID &&
             tree_->ReadLeaf(page_id_, &entries_, &right_sibling_page_id_).ok();
    while (valid_ && entries_.empty()) {
      if (right_sibling_page_id_ == INVALID_PAGE_ID) {
        valid_ = false;
        return;
      }
      page_id_ = right_sibling_page_id_;
      valid_ = tree_->ReadLeaf(page_id_, &entries_, &right_sibling_page_id_).ok();
    }
  }

  const BPlusTreeEngine* tree_;
  page_id_t page_id_;
  page_id_t right_sibling_page_id_ = INVALID_PAGE_ID;
  std::size_t index_;
  std::vector<LeafEntry> entries_;
  bool valid_ = false;
};

BPlusTreeEngine::BPlusTreeEngine(DiskManager* disk_manager)
    : disk_manager_(disk_manager), initialization_status_(InitializeOrLoadMetadata()) {}

Status BPlusTreeEngine::InitializeOrLoadMetadata() {
  if (disk_manager_ == nullptr) {
    return Status::InvalidArgument("BPlusTreeEngine requires a DiskManager");
  }
  if (disk_manager_->GetNumPages() == 0) {
    if (disk_manager_->AllocatePage() != 0) {
      return Status::Corruption("metadata page was not allocated at page zero");
    }
    Page page;
    SlottedPage metadata(&page);
    metadata.Initialize(PageType::kMeta);
    std::array<char, 8> record{};
    std::copy(kMetadataMagic.begin(), kMetadataMagic.end(), record.begin());
    WriteU32(record.data() + 4, static_cast<uint32_t>(INVALID_PAGE_ID));
    Status status = metadata.InsertCell(0, record);
    if (!status.ok()) {
      return status;
    }
    return disk_manager_->WritePage(0, page.GetData());
  }

  Page page;
  Status status = disk_manager_->ReadPage(0, page.GetData());
  if (!status.ok()) {
    return status;
  }
  SlottedPage metadata(&page);
  if (metadata.GetPageType() != PageType::kMeta || !metadata.VerifyChecksum() ||
      metadata.NumCells() != 1) {
    return Status::Corruption("invalid B+Tree metadata page");
  }
  const std::span<const char> record = metadata.GetCell(0);
  if (record.size() != 8 ||
      !std::equal(kMetadataMagic.begin(), kMetadataMagic.end(), record.begin())) {
    return Status::Corruption("invalid B+Tree metadata record");
  }
  root_page_id_ = static_cast<page_id_t>(ReadU32(record.data() + 4));
  return Status::OK();
}

Status BPlusTreeEngine::StoreRootPageId(page_id_t root_page_id) {
  Page page;
  Status status = disk_manager_->ReadPage(0, page.GetData());
  if (!status.ok()) {
    return status;
  }
  SlottedPage metadata(&page);
  if (metadata.GetPageType() != PageType::kMeta || !metadata.VerifyChecksum() ||
      metadata.NumCells() != 1) {
    return Status::Corruption("cannot update invalid B+Tree metadata page");
  }
  std::array<char, 8> record{};
  std::copy(kMetadataMagic.begin(), kMetadataMagic.end(), record.begin());
  WriteU32(record.data() + 4, static_cast<uint32_t>(root_page_id));
  status = metadata.DeleteCell(0);
  if (!status.ok()) {
    return status;
  }
  status = metadata.InsertCell(0, record);
  if (!status.ok()) {
    return status;
  }
  status = disk_manager_->WritePage(0, page.GetData());
  if (status.ok()) {
    root_page_id_ = root_page_id;
  }
  return status;
}

Status BPlusTreeEngine::ReadLeaf(page_id_t page_id,
                                  std::vector<LeafEntry>* entries,
                                  page_id_t* right_sibling_page_id) const {
  if (entries == nullptr || right_sibling_page_id == nullptr) {
    return Status::InvalidArgument("ReadLeaf requires output parameters");
  }
  Page page;
  Status status = disk_manager_->ReadPage(page_id, page.GetData());
  if (!status.ok()) {
    return status;
  }
  SlottedPage slotted(&page);
  if (slotted.GetPageType() != PageType::kLeaf || !slotted.VerifyChecksum()) {
    return Status::Corruption("invalid leaf page");
  }
  entries->clear();
  entries->reserve(slotted.NumCells());
  for (uint16_t i = 0; i < slotted.NumCells(); ++i) {
    LeafCell cell;
    status = DecodeLeafCell(slotted.GetCell(i), &cell);
    if (!status.ok()) {
      return status;
    }
    entries->push_back({std::move(cell.key), std::move(cell.value)});
  }
  *right_sibling_page_id = slotted.RightSiblingPageId();
  return Status::OK();
}

Status BPlusTreeEngine::WriteLeaf(page_id_t page_id,
                                   const std::vector<LeafEntry>& entries,
                                   page_id_t right_sibling_page_id) {
  Page page;
  SlottedPage slotted(&page);
  slotted.Initialize(PageType::kLeaf, right_sibling_page_id);
  for (uint16_t i = 0; i < entries.size(); ++i) {
    std::vector<char> cell;
    Status status = EncodeLeafCell(entries[i].key, entries[i].value, &cell);
    if (!status.ok()) {
      return status;
    }
    status = slotted.InsertCell(i, cell);
    if (!status.ok()) {
      return PageFull(status);
    }
  }
  return disk_manager_->WritePage(page_id, page.GetData());
}

Status BPlusTreeEngine::ReadInternal(page_id_t page_id, InternalNodeData* node) const {
  if (node == nullptr) {
    return Status::InvalidArgument("ReadInternal requires an output parameter");
  }
  Page page;
  Status status = disk_manager_->ReadPage(page_id, page.GetData());
  if (!status.ok()) {
    return status;
  }
  SlottedPage slotted(&page);
  if (slotted.GetPageType() != PageType::kInternal || !slotted.VerifyChecksum()) {
    return Status::Corruption("invalid internal page");
  }
  const page_id_t leftmost_child = slotted.RightSiblingPageId();
  if (leftmost_child == INVALID_PAGE_ID) {
    return Status::Corruption("internal page has no leftmost child");
  }
  node->keys.clear();
  node->children.clear();
  node->children.push_back(leftmost_child);
  for (uint16_t i = 0; i < slotted.NumCells(); ++i) {
    InternalCell cell;
    status = DecodeInternalCell(slotted.GetCell(i), &cell);
    if (!status.ok()) {
      return status;
    }
    node->keys.push_back(std::move(cell.key));
    node->children.push_back(cell.child_page_id);
  }
  if (node->children.size() != node->keys.size() + 1 ||
      !std::is_sorted(node->keys.begin(), node->keys.end())) {
    return Status::Corruption("internal page has invalid separators");
  }
  return Status::OK();
}

Status BPlusTreeEngine::WriteInternal(page_id_t page_id, const InternalNodeData& node) {
  if (node.children.size() != node.keys.size() + 1 || node.children.empty()) {
    return Status::InvalidArgument("internal node has invalid child count");
  }
  Page page;
  SlottedPage slotted(&page);
  // For internal pages this header field stores the leftmost child. Leaf pages
  // use the same bytes as their right-sibling pointer.
  slotted.Initialize(PageType::kInternal, node.children.front());
  for (uint16_t i = 0; i < node.keys.size(); ++i) {
    std::vector<char> cell;
    Status status = EncodeInternalCell(node.keys[i], node.children[i + 1], &cell);
    if (!status.ok()) {
      return status;
    }
    status = slotted.InsertCell(i, cell);
    if (!status.ok()) {
      return PageFull(status);
    }
  }
  return disk_manager_->WritePage(page_id, page.GetData());
}

Status BPlusTreeEngine::FindLeaf(const std::string& key, page_id_t* leaf_page_id) const {
  if (leaf_page_id == nullptr) {
    return Status::InvalidArgument("leaf_page_id must not be null");
  }
  if (root_page_id_ == INVALID_PAGE_ID) {
    return Status::NotFound("tree is empty");
  }
  page_id_t current = root_page_id_;
  while (true) {
    Page page;
    Status status = disk_manager_->ReadPage(current, page.GetData());
    if (!status.ok()) {
      return status;
    }
    SlottedPage slotted(&page);
    if (!slotted.VerifyChecksum()) {
      return Status::Corruption("page checksum mismatch during search");
    }
    if (slotted.GetPageType() == PageType::kLeaf) {
      *leaf_page_id = current;
      return Status::OK();
    }
    if (slotted.GetPageType() != PageType::kInternal) {
      return Status::Corruption("non-tree page reached during search");
    }
    InternalNodeData internal;
    status = ReadInternal(current, &internal);
    if (!status.ok()) {
      return status;
    }
    const auto it = std::upper_bound(internal.keys.begin(), internal.keys.end(), key);
    current = internal.children[static_cast<std::size_t>(it - internal.keys.begin())];
  }
}

Status BPlusTreeEngine::Get(const std::string& key, std::string* value_out) {
  if (!initialization_status_.ok()) {
    return initialization_status_;
  }
  if (value_out == nullptr) {
    return Status::InvalidArgument("value_out must not be null");
  }
  page_id_t leaf_page_id;
  Status status = FindLeaf(key, &leaf_page_id);
  if (!status.ok()) {
    return status;
  }
  std::vector<LeafEntry> entries;
  page_id_t ignored_sibling;
  status = ReadLeaf(leaf_page_id, &entries, &ignored_sibling);
  if (!status.ok()) {
    return status;
  }
  const auto it = std::lower_bound(entries.begin(), entries.end(), key,
                                   [](const LeafEntry& entry, const std::string& value) {
                                     return entry.key < value;
                                   });
  if (it == entries.end() || it->key != key) {
    return Status::NotFound("key not found");
  }
  *value_out = it->value;
  return Status::OK();
}

Status BPlusTreeEngine::InsertRecursive(page_id_t page_id, const std::string& key,
                                         const std::string& value,
                                         std::optional<Split>* split_out) {
  *split_out = std::nullopt;
  Page page;
  Status status = disk_manager_->ReadPage(page_id, page.GetData());
  if (!status.ok()) {
    return status;
  }
  SlottedPage slotted(&page);
  if (!slotted.VerifyChecksum()) {
    return Status::Corruption("page checksum mismatch during insert");
  }
  if (slotted.GetPageType() == PageType::kLeaf) {
    std::vector<LeafEntry> entries;
    page_id_t sibling;
    status = ReadLeaf(page_id, &entries, &sibling);
    if (!status.ok()) {
      return status;
    }
    const auto it = std::lower_bound(entries.begin(), entries.end(), key,
                                     [](const LeafEntry& entry, const std::string& value) {
                                       return entry.key < value;
                                     });
    if (it != entries.end() && it->key == key) {
      it->value = value;
    } else {
      entries.insert(it, {key, value});
    }
    status = WriteLeaf(page_id, entries, sibling);
    if (status.ok()) {
      return status;
    }
    if (status.code() != Status::Code::kInvalidArgument || entries.size() < 2) {
      return status;
    }
    const std::size_t split_at = entries.size() / 2;
    std::vector<LeafEntry> right_entries(
        std::make_move_iterator(entries.begin() + static_cast<std::ptrdiff_t>(split_at)),
        std::make_move_iterator(entries.end()));
    entries.resize(split_at);
    const page_id_t right_page_id = disk_manager_->AllocatePage();
    status = WriteLeaf(page_id, entries, right_page_id);
    if (!status.ok()) {
      return status;
    }
    status = WriteLeaf(right_page_id, right_entries, sibling);
    if (!status.ok()) {
      return status;
    }
    *split_out = Split{right_entries.front().key, right_page_id};
    return Status::OK();
  }
  if (slotted.GetPageType() != PageType::kInternal) {
    return Status::Corruption("non-tree page reached during insert");
  }

  InternalNodeData internal;
  status = ReadInternal(page_id, &internal);
  if (!status.ok()) {
    return status;
  }
  const auto child_it = std::upper_bound(internal.keys.begin(), internal.keys.end(), key);
  const std::size_t child_index =
      static_cast<std::size_t>(child_it - internal.keys.begin());
  std::optional<Split> child_split;
  status = InsertRecursive(internal.children[child_index], key, value, &child_split);
  if (!status.ok() || !child_split) {
    return status;
  }
  internal.keys.insert(internal.keys.begin() + static_cast<std::ptrdiff_t>(child_index),
                       std::move(child_split->separator));
  internal.children.insert(
      internal.children.begin() + static_cast<std::ptrdiff_t>(child_index + 1),
      child_split->right_page_id);
  status = WriteInternal(page_id, internal);
  if (status.ok()) {
    return status;
  }
  if (status.code() != Status::Code::kInvalidArgument || internal.keys.size() < 2) {
    return status;
  }
  const std::size_t promote_index = internal.keys.size() / 2;
  const std::string separator = internal.keys[promote_index];
  InternalNodeData right;
  right.keys.assign(std::make_move_iterator(internal.keys.begin() +
                                             static_cast<std::ptrdiff_t>(promote_index + 1)),
                    std::make_move_iterator(internal.keys.end()));
  right.children.assign(std::make_move_iterator(internal.children.begin() +
                                                 static_cast<std::ptrdiff_t>(promote_index + 1)),
                        std::make_move_iterator(internal.children.end()));
  internal.keys.resize(promote_index);
  internal.children.resize(promote_index + 1);
  const page_id_t right_page_id = disk_manager_->AllocatePage();
  status = WriteInternal(page_id, internal);
  if (!status.ok()) {
    return status;
  }
  status = WriteInternal(right_page_id, right);
  if (!status.ok()) {
    return status;
  }
  *split_out = Split{separator, right_page_id};
  return Status::OK();
}

Status BPlusTreeEngine::Put(const std::string& key, const std::string& value) {
  if (!initialization_status_.ok()) {
    return initialization_status_;
  }
  if (root_page_id_ == INVALID_PAGE_ID) {
    const page_id_t root_page_id = disk_manager_->AllocatePage();
    Status status = WriteLeaf(root_page_id, {{key, value}}, INVALID_PAGE_ID);
    if (!status.ok()) {
      return status;
    }
    return StoreRootPageId(root_page_id);
  }
  std::optional<Split> split;
  Status status = InsertRecursive(root_page_id_, key, value, &split);
  if (!status.ok() || !split) {
    return status;
  }
  const page_id_t new_root_page_id = disk_manager_->AllocatePage();
  InternalNodeData root{{split->separator}, {root_page_id_, split->right_page_id}};
  status = WriteInternal(new_root_page_id, root);
  if (!status.ok()) {
    return status;
  }
  return StoreRootPageId(new_root_page_id);
}

Status BPlusTreeEngine::Delete(const std::string& key) {
  if (!initialization_status_.ok()) {
    return initialization_status_;
  }
  page_id_t leaf_page_id;
  Status status = FindLeaf(key, &leaf_page_id);
  if (!status.ok()) {
    return status;
  }
  std::vector<LeafEntry> entries;
  page_id_t sibling;
  status = ReadLeaf(leaf_page_id, &entries, &sibling);
  if (!status.ok()) {
    return status;
  }
  const auto it = std::lower_bound(entries.begin(), entries.end(), key,
                                   [](const LeafEntry& entry, const std::string& value) {
                                     return entry.key < value;
                                   });
  if (it == entries.end() || it->key != key) {
    return Status::NotFound("key not found");
  }
  entries.erase(it);
  // Rebalancing and page reclamation after a tree delete are deferred. The
  // storage free-list is nevertheless available to merge/vacuum code.
  return WriteLeaf(leaf_page_id, entries, sibling);
}

std::unique_ptr<Iterator> BPlusTreeEngine::Scan(const std::string& start_key) {
  if (!initialization_status_.ok() || root_page_id_ == INVALID_PAGE_ID) {
    return std::make_unique<IteratorImpl>(this, INVALID_PAGE_ID, 0);
  }
  page_id_t leaf_page_id;
  if (!FindLeaf(start_key, &leaf_page_id).ok()) {
    return std::make_unique<IteratorImpl>(this, INVALID_PAGE_ID, 0);
  }
  std::vector<LeafEntry> entries;
  page_id_t ignored_sibling;
  if (!ReadLeaf(leaf_page_id, &entries, &ignored_sibling).ok()) {
    return std::make_unique<IteratorImpl>(this, INVALID_PAGE_ID, 0);
  }
  const auto it = std::lower_bound(entries.begin(), entries.end(), start_key,
                                   [](const LeafEntry& entry, const std::string& value) {
                                     return entry.key < value;
                                   });
  return std::make_unique<IteratorImpl>(
      this, leaf_page_id, static_cast<std::size_t>(it - entries.begin()));
}

bool BPlusTreeEngine::ValidatePage(page_id_t page_id, std::size_t depth,
                                   std::size_t* leaf_depth, std::string* last_key,
                                   page_id_t* expected_leaf_page_id,
                                   std::string* error_out) const {
  const auto fail = [error_out](const std::string& message) {
    if (error_out != nullptr) {
      *error_out = message;
    }
    return false;
  };
  Page page;
  if (!disk_manager_->ReadPage(page_id, page.GetData()).ok()) {
    return fail("cannot read tree page");
  }
  SlottedPage slotted(&page);
  if (!slotted.VerifyChecksum()) {
    return fail("tree page checksum mismatch");
  }
  if (slotted.GetPageType() == PageType::kLeaf) {
    std::vector<LeafEntry> entries;
    page_id_t sibling;
    if (!ReadLeaf(page_id, &entries, &sibling).ok()) {
      return fail("invalid leaf page");
    }
    if (*expected_leaf_page_id != INVALID_PAGE_ID && page_id != *expected_leaf_page_id) {
      return fail("leaf sibling chain differs from tree order");
    }
    for (const LeafEntry& entry : entries) {
      if (!last_key->empty() && *last_key >= entry.key) {
        return fail("leaf keys are not globally sorted");
      }
      *last_key = entry.key;
    }
    if (*leaf_depth == 0) {
      *leaf_depth = depth;
    } else if (*leaf_depth != depth) {
      return fail("leaves are not at a common depth");
    }
    *expected_leaf_page_id = sibling;
    return true;
  }
  if (slotted.GetPageType() != PageType::kInternal) {
    return fail("unexpected page type in tree");
  }
  InternalNodeData internal;
  if (!ReadInternal(page_id, &internal).ok()) {
    return fail("invalid internal page");
  }
  for (page_id_t child : internal.children) {
    if (!ValidatePage(child, depth + 1, leaf_depth, last_key, expected_leaf_page_id,
                      error_out)) {
      return false;
    }
  }
  return true;
}

bool BPlusTreeEngine::ValidateInvariants(std::string* error_out) const {
  if (error_out != nullptr) {
    error_out->clear();
  }
  if (!initialization_status_.ok()) {
    if (error_out != nullptr) {
      *error_out = initialization_status_.message();
    }
    return false;
  }
  if (root_page_id_ == INVALID_PAGE_ID) {
    return true;
  }
  std::size_t leaf_depth = 0;
  std::string last_key;
  page_id_t expected_leaf_page_id = INVALID_PAGE_ID;
  return ValidatePage(root_page_id_, 1, &leaf_depth, &last_key,
                      &expected_leaf_page_id, error_out);
}

std::size_t BPlusTreeEngine::Height() const {
  if (!initialization_status_.ok() || root_page_id_ == INVALID_PAGE_ID) {
    return 0;
  }
  std::size_t height = 0;
  page_id_t current = root_page_id_;
  while (current != INVALID_PAGE_ID) {
    ++height;
    Page page;
    if (!disk_manager_->ReadPage(current, page.GetData()).ok()) {
      return 0;
    }
    SlottedPage slotted(&page);
    if (!slotted.VerifyChecksum() || slotted.GetPageType() == PageType::kLeaf) {
      return slotted.GetPageType() == PageType::kLeaf ? height : 0;
    }
    if (slotted.GetPageType() != PageType::kInternal) {
      return 0;
    }
    current = slotted.RightSiblingPageId();
  }
  return 0;
}

}  // namespace dbengine
