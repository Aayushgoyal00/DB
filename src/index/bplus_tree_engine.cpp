#include "index/bplus_tree_engine.h"

namespace dbengine {

BPlusTreeEngine::BPlusTreeEngine(DiskManager* disk_manager)
    : disk_manager_(disk_manager) {}

Status BPlusTreeEngine::Get(const std::string& /*key*/,
                             std::string* /*value_out*/) {
  // TODO(Phase 0): in-memory search over leaf/internal node structs.
  // TODO(Phase 2): swap in-memory node pointers for page_id lookups
  // through disk_manager_.
  return Status::NotImplemented("BPlusTreeEngine::Get");
}

Status BPlusTreeEngine::Put(const std::string& /*key*/,
                             const std::string& /*value*/) {
  // TODO(Phase 0): insert + split-on-overflow, in-memory.
  // TODO(Phase 2): persist splits through DiskManager, propagate new
  // separator keys up to parent pages.
  return Status::NotImplemented("BPlusTreeEngine::Put");
}

Status BPlusTreeEngine::Delete(const std::string& /*key*/) {
  // TODO(Phase 0): delete + merge/redistribute-on-underflow, in-memory.
  return Status::NotImplemented("BPlusTreeEngine::Delete");
}

std::unique_ptr<Iterator> BPlusTreeEngine::Scan(
    const std::string& /*start_key*/) {
  // TODO(Phase 2): leaf-level sibling-pointer iteration.
  return nullptr;
}

} // namespace dbengine
