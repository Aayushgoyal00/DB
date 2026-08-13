#pragma once

#include <cstring>

#include "common/config.h"

namespace dbengine {

// In-memory representation of one on-disk page. This is what lives inside a
// buffer pool frame (Phase 3) and what DiskManager reads/writes raw bytes
// into. It deliberately knows nothing about B+Tree structure — page-layout
// concerns (slotted pages, cell format) belong in Phase 1/2 code that reads
// and writes through GetData(), not in this class.
class Page {
 public:
  Page() { ResetMemory(); }

  char* GetData() { return data_; }
  const char* GetData() const { return data_; }

  page_id_t GetPageId() const { return page_id_; }
  void SetPageId(page_id_t page_id) { page_id_ = page_id; }

  bool IsDirty() const { return is_dirty_; }
  void SetDirty(bool dirty) { is_dirty_ = dirty; }

  int PinCount() const { return pin_count_; }
  void IncPinCount() { ++pin_count_; }
  void DecPinCount() {
    if (pin_count_ > 0) --pin_count_;
  }

  void ResetMemory() { std::memset(data_, 0, PAGE_SIZE); }

 private:
  char data_[PAGE_SIZE];
  page_id_t page_id_ = INVALID_PAGE_ID;
  bool is_dirty_ = false;
  int pin_count_ = 0;

  // Phase 3 will add a std::shared_mutex here for latch-based concurrency
  // control (see ARCHITECTURE.md, Phase 3). Left out of Phase 0 on purpose —
  // no point synchronizing single-threaded code.
};

} // namespace dbengine
