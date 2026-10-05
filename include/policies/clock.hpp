#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

#include "internal/datastructures/queue.hpp"
#include "internal/eviction/eviction_policy.hpp"

namespace cachesim::policies {

// CLOCK, a.k.a. second-chance FIFO: LRU's hit ratio at FIFO's cost.
//
// Instead of moving an object on every hit (two pointer writes plus a
// potential cache miss on the neighbouring nodes, on the hot path), a hit
// just sets a counter in the object's own node. The cost is paid lazily, at
// eviction time: the scan starts at the queue's tail and gives any object
// with a non-zero counter a second chance — decrement it and move it to the
// front — until it finds one at zero, which it evicts.
//
// The scan looks unbounded but is amortized O(1): each iteration consumes one
// counter increment, and increments only happen on hits, so the total scan
// work over a run is bounded by the number of hits. This is why CLOCK is what
// real operating systems and databases actually implement, and why it is the
// right default when the hot path matters more than the last half point of
// hit ratio.
//
// Parameter:
//   n-bit-counter=K (default 1) — how many chances an object can bank.
//     K=1 is textbook CLOCK (one reference bit). Larger K approximates LFU's
//     behaviour on frequency-skewed traces at the same cost, since a hot
//     object can survive several passes of the hand.
class Clock final : public IEvictionPolicy {
 public:
  explicit Clock(const PolicyConfig& config = {})
      : queue_(config.entry_hint),
        max_counter_(static_cast<std::uint8_t>(
            std::min<std::int64_t>(255, std::max<std::int64_t>(1, config.params.getInt("n-bit-counter", 1))))) {}

  [[nodiscard]] std::string name() const override {
    return max_counter_ == 1 ? "Clock" : "Clock-" + std::to_string(max_counter_);
  }

  Status onAdmit(internal::CacheEntry* entry, const Request& req) override {
    (void)req;
    const Status status = queue_.pushFront(entry);
    if (!ok(status)) return status;
    // Admitted objects start with no credit, so a one-hit-wonder is evicted
    // on the hand's first pass rather than surviving it.
    queue_.payload(entry) = 0;
    return Status::kSuccess;
  }

  Status onHit(internal::CacheEntry* entry, const Request& req) override {
    (void)req;
    if (!queue_.contains(entry)) return Status::kFailure;
    std::uint8_t& counter = queue_.payload(entry);
    if (counter < max_counter_) ++counter;
    return Status::kSuccess;
  }

  internal::CacheEntry* evict() override {
    for (;;) {
      internal::CacheEntry* candidate = queue_.back();
      if (candidate == nullptr) return nullptr;
      std::uint8_t& counter = queue_.payload(candidate);
      if (counter == 0) return queue_.popBack();
      --counter;
      // Second chance: back to the front of the queue, where it has a full
      // lap before the hand sees it again.
      if (!ok(queue_.moveToFront(candidate))) return nullptr;
    }
  }

  Status onRemove(internal::CacheEntry* entry) override { return queue_.unlink(entry); }

  Status onResize(internal::CacheEntry* entry, std::uint32_t old_size,
                  std::uint32_t new_size) override {
    (void)entry;
    queue_.adjustBytes(static_cast<std::int64_t>(new_size) - static_cast<std::int64_t>(old_size));
    return Status::kSuccess;
  }

  void clear() override { queue_.clear(); }

  [[nodiscard]] std::uint64_t metadataBytes() const override { return queue_.metadataBytes(); }

 private:
  Queue<std::uint8_t> queue_;
  std::uint8_t max_counter_;
};

}  // namespace cachesim::policies
