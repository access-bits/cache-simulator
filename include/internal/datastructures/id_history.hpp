#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>

#include "ankerl/unordered_dense.h"

namespace cachesim {

// Bounded-capacity record of recently-seen object ids, with O(1) membership
// checks and FIFO eviction of the oldest id once full. For "ghost" tracking
// (ARC/2Q/S3FIFO all remember objects recently evicted from the real
// cache) — there's no live CacheEntry for an evicted object to attach
// anything to, so this works directly with obj_id instead of CacheEntry*.
class IdHistory {
 public:
  explicit IdHistory(std::size_t capacity) : capacity_(capacity) {}

  bool contains(std::uint64_t obj_id) const { return ids_.contains(obj_id); }

  // Records obj_id, evicting the oldest recorded id if at capacity.
  // No-op if obj_id is already recorded.
  void record(std::uint64_t obj_id) {
    if (capacity_ == 0 || ids_.contains(obj_id)) return;
    if (ids_.size() >= capacity_) {
      // order_ may also hold stale ids already removed via erase(); skip those.
      while (!order_.empty() && !ids_.contains(order_.front())) {
        order_.pop_front();
      }
      if (ids_.size() >= capacity_ && !order_.empty()) {
        ids_.erase(order_.front());
        order_.pop_front();
      }
    }
    ids_.insert(obj_id);
    order_.push_back(obj_id);
  }

  // No-op if obj_id isn't recorded. Leaves a stale entry in the FIFO order
  // (cheaply skipped later by record()) rather than scanning to remove it.
  void erase(std::uint64_t obj_id) { ids_.erase(obj_id); }

  std::size_t size() const { return ids_.size(); }
  std::size_t capacity() const { return capacity_; }

 private:
  std::size_t capacity_;
  ankerl::unordered_dense::set<std::uint64_t> ids_;
  std::deque<std::uint64_t> order_;
};

}  // namespace cachesim
