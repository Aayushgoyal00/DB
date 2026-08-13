#pragma once

#include "index/kv_store.h"
#include "storage/disk_manager.h"

namespace dbengine {

// Placeholder engine. Right now this only proves the KVStore interface
// compiles and links end to end; the actual node structs, search, split,
// and merge logic get filled in across Phase 0 (in-memory only) through
// Phase 2 (persistent, page-backed). See ARCHITECTURE.md sections 4-6.
class BPlusTreeEngine : public KVStore {
 public:
  explicit BPlusTreeEngine(DiskManager* disk_manager);

  Status Get(const std::string& key, std::string* value_out) override;
  Status Put(const std::string& key, const std::string& value) override;
  Status Delete(const std::string& key) override;
  std::unique_ptr<Iterator> Scan(const std::string& start_key) override;

 private:
  DiskManager* disk_manager_; // not owned
  page_id_t root_page_id_ = INVALID_PAGE_ID;
};

} // namespace dbengine
