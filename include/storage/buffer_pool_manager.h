#pragma once

#include <cstddef>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "common/config.h"
#include "common/status.h"
#include "storage/disk_manager.h"
#include "storage/log_manager.h"
#include "storage/page.h"
#include "storage/replacer.h"

namespace dbengine {

// BufferPoolManager — caches Page objects in memory so the engine doesn't
// touch disk on every Get/Put. Sits between any KVStore implementation
// (BPlusTreeEngine, future LsmEngine) and DiskManager.
//
// Contract:
//   FetchPage/NewPage return a pinned Page*. Caller MUST call UnpinPage
//   when done (with is_dirty=true if it wrote). Pages with pin_count>0
//   are never evictable. This is the load-bearing invariant — violating
//   it gives use-after-free the moment eviction flushes the frame.
class BufferPoolManager {
 public:
  BufferPoolManager(size_t pool_size, DiskManager* disk_manager,
                    LogManager* log_manager = nullptr);
  ~BufferPoolManager();

  BufferPoolManager(const BufferPoolManager&) = delete;
  BufferPoolManager& operator=(const BufferPoolManager&) = delete;

  // Fetch a page from cache. Loads from disk on miss; pins on success.
  // Returns nullptr only if the page is in the pool table but pinned
  // inconsistently (shouldn't happen) or if the pool is fully pinned and
  // no victim exists (returns nullptr — caller may retry).
  Page* FetchPage(page_id_t page_id);

  // Release a pin. If is_dirty is true, the page is marked dirty and will
  // be flushed on eviction. Decrements pin_count; if it hits 0 the
  // frame becomes evictable.
  bool UnpinPage(page_id_t page_id, bool is_dirty);

  // Force a single page to disk (regardless of dirty/lru state).
  Status FlushPage(page_id_t page_id);

  // Flush every dirty page to disk. Used on graceful shutdown.
  Status FlushAllPages();

  // Allocate a brand-new page, zero it, pin it, and return it.
  // The new page_id is written to *page_id_out.
  Page* NewPage(page_id_t* page_id_out);

  // Delete a page from cache and mark its disk page reusable.
  // No-op if the page is currently pinned (you can't delete what someone
  // is using).
  bool DeletePage(page_id_t page_id);

  size_t PoolSize() const { return pool_size_; }

 private:
  // Find a free frame: from free_list_ first, else evict via replacer_.
  // Returns INVALID_FRAME_ID if no victim available.
  frame_id_t FindReplacementFrame();

  size_t pool_size_;
  Page* pages_;
  std::unordered_map<page_id_t, frame_id_t> page_table_;
  std::list<frame_id_t> free_list_;
  std::unique_ptr<Replacer> replacer_;
  DiskManager* disk_manager_;
  LogManager* log_manager_;  // not owned; optional but required for WAL

  std::mutex latch_;

  static constexpr frame_id_t INVALID_FRAME_ID = -1;
};

}  // namespace dbengine
