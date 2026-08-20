#pragma once

#include <cstddef>
#include <vector>

#include "common/config.h"

namespace dbengine {

// Eviction policy interface. The buffer pool asks for a victim when it
// runs out of free frames; we need to know which unpinned frame to evict.
// Implementations: ClockReplacer (Phase 3 default), LRUKReplacer (later).
class Replacer {
 public:
  virtual ~Replacer() = default;

  // Record that frame was accessed — gives it a "second chance" under
  // clock, equivalent to moving-to-head under LRU.
  virtual void RecordAccess(frame_id_t frame_id) = 0;

  // Mark a frame as evictable (pin dropped to 0). The replacer is now
  // allowed to return it as a victim.
  virtual void SetEvictable(frame_id_t frame_id, bool evictable) = 0;

  // Remove frame from replacer entirely (e.g. the page was deleted and
  // the frame will be reused). Returns true if it was tracked.
  virtual bool Remove(frame_id_t frame_id) = 0;

  // Pick a victim frame to evict. Returns true on success; false if no
  // evictable frame exists (everyone is pinned → caller must wait / fail).
  virtual bool Victim(frame_id_t* frame_id_out) = 0;

  virtual size_t Size() = 0;
};

// Second-chance / clock-sweep eviction. One ref bit per frame; the clock
// hand sweeps forward, clearing bits it sees set, and picks the first
// frame whose bit is already clear. O(1) amortized, no heap, lockable.
class ClockReplacer : public Replacer {
 public:
  explicit ClockReplacer(size_t num_frames);

  void RecordAccess(frame_id_t frame_id) override;
  void SetEvictable(frame_id_t frame_id, bool evictable) override;
  bool Remove(frame_id_t frame_id) override;
  bool Victim(frame_id_t* frame_id_out) override;
  size_t Size() override;

 private:
  struct Entry {
    bool in_use = false;
    bool ref_bit = false;
  };

  std::vector<Entry> entries_;
  size_t hand_ = 0;
  size_t evictable_count_ = 0;
};

}  // namespace dbengine
