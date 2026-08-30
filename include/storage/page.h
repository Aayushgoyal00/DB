#pragma once

#include <cstring>
#include <shared_mutex>

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

  std::shared_mutex& Latch() { return latch_; }
  const std::shared_mutex& Latch() const { return latch_; }

  char* GetData() { return data_; }
  const char* GetData() const { return data_; }

  page_id_t GetPageId() const { return page_id_; }
  void SetPageId(page_id_t page_id) { page_id_ = page_id; }

  bool IsDirty() const { return is_dirty_; }
  void SetDirty(bool dirty) { is_dirty_ = dirty; }

  lsn_t GetPageLSN() const { return page_lsn_; }
  void SetPageLSN(lsn_t lsn) { page_lsn_ = lsn; }

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
  lsn_t page_lsn_ = INVALID_LSN;

  // Per-page reader/writer latch. Pinning (BufferPoolManager) keeps the
  // frame alive; this latch keeps the page bytes consistent while callers
  // read or write them. mutable so const GetData() can still lock for read.
  mutable std::shared_mutex latch_;
};

} // namespace dbengine
