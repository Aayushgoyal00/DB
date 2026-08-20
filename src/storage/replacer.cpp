#include "storage/replacer.h"

#include <mutex>

namespace dbengine {

ClockReplacer::ClockReplacer(size_t num_frames) : entries_(num_frames) {}

void ClockReplacer::RecordAccess(frame_id_t frame_id) {
  if (frame_id < 0 || static_cast<size_t>(frame_id) >= entries_.size()) return;
  entries_[frame_id].ref_bit = true;
}

void ClockReplacer::SetEvictable(frame_id_t frame_id, bool evictable) {
  if (frame_id < 0 || static_cast<size_t>(frame_id) >= entries_.size()) return;
  auto& e = entries_[frame_id];
  if (e.in_use && !evictable) {
    e.in_use = false;
    --evictable_count_;
  } else if (!e.in_use && evictable) {
    e.in_use = true;
    ++evictable_count_;
    e.ref_bit = false;
  }
}

bool ClockReplacer::Remove(frame_id_t frame_id) {
  if (frame_id < 0 || static_cast<size_t>(frame_id) >= entries_.size()) return false;
  auto& e = entries_[frame_id];
  if (!e.in_use) return false;
  e.in_use = false;
  e.ref_bit = false;
  --evictable_count_;
  return true;
}

bool ClockReplacer::Victim(frame_id_t* frame_id_out) {
  if (evictable_count_ == 0) return false;

  while (true) {
    auto& e = entries_[hand_];
    if (e.in_use && e.ref_bit) {
      e.ref_bit = false;
    } else if (e.in_use && !e.ref_bit) {
      *frame_id_out = static_cast<frame_id_t>(hand_);
      e.in_use = false;
      --evictable_count_;
      hand_ = (hand_ + 1) % entries_.size();
      return true;
    }
    hand_ = (hand_ + 1) % entries_.size();
  }
}

size_t ClockReplacer::Size() { return evictable_count_; }

}  // namespace dbengine
