#pragma once

#include <cstddef>
#include <string>

#include "internal/datastructures/queue.hpp"
#include "internal/eviction/eviction_policy.hpp"

namespace cachesim::policies {

// SIEVE (Zhang et al., NSDI'24): CLOCK's cheap hot path, but the hand never
// moves an object.
//
// The difference from CLOCK is one line and it matters. CLOCK gives a
// surviving object a second chance by moving it to the front of the queue, so
// a hot object keeps being promoted and the queue order drifts towards
// recency. SIEVE leaves it exactly where it is and only advances the hand.
// The consequence is that objects admitted long ago but still in use sit near
// the tail as a stable "sieve" that newly admitted one-hit-wonders have to
// pass through — new objects are inserted at the head and must survive a full
// sweep to stay, so scan traffic is filtered out instead of flushing the
// working set.
//
// The hand starts at the tail (oldest) and walks towards the head, wrapping
// back to the tail when it runs off the end. "visited" is one bit per object,
// set on a hit, cleared by the hand.
class Sieve final : public IEvictionPolicy {
 public:
  explicit Sieve(const PolicyConfig& config = {}) : queue_(config.entry_hint) {}

  [[nodiscard]] std::string name() const override { return "Sieve"; }

  Status onAdmit(internal::CacheEntry* entry, const Request& req) override {
    (void)req;
    const Status status = queue_.pushFront(entry);
    if (!ok(status)) return status;
    queue_.payload(entry) = false;
    return Status::kSuccess;
  }

  Status onHit(internal::CacheEntry* entry, const Request& req) override {
    (void)req;
    if (!queue_.contains(entry)) return Status::kFailure;
    queue_.payload(entry) = true;
    return Status::kSuccess;
  }

  internal::CacheEntry* evict() override {
    if (queue_.empty()) return nullptr;
    internal::CacheEntry* candidate = hand_ != nullptr ? hand_ : queue_.back();
    // Bounded by one full lap. If every object is marked, the lap clears
    // every bit and the hand evicts whatever it is standing on when the lap
    // ends — which is the paper's behaviour, and the bound is what stops the
    // sweep spinning on a cache where everything was hit since the last
    // eviction.
    for (std::size_t steps = 0, limit = queue_.size(); steps < limit; ++steps) {
      if (candidate == nullptr) candidate = queue_.back();  // wrapped past the head
      if (!queue_.payload(candidate)) break;
      queue_.payload(candidate) = false;
      candidate = queue_.prev(candidate);  // towards the head
    }
    if (candidate == nullptr) candidate = queue_.back();
    if (candidate == nullptr) return nullptr;
    // The hand resumes one step further towards the head next time, rather
    // than restarting the sweep at the tail.
    hand_ = queue_.prev(candidate);
    if (!ok(queue_.unlink(candidate))) return nullptr;
    return candidate;
  }

  Status onRemove(internal::CacheEntry* entry) override {
    if (entry == hand_) hand_ = queue_.prev(entry);
    return queue_.unlink(entry);
  }

  Status onResize(internal::CacheEntry* entry, std::uint32_t old_size,
                  std::uint32_t new_size) override {
    (void)entry;
    queue_.adjustBytes(static_cast<std::int64_t>(new_size) - static_cast<std::int64_t>(old_size));
    return Status::kSuccess;
  }

  void clear() override {
    queue_.clear();
    hand_ = nullptr;
  }

  [[nodiscard]] std::uint64_t metadataBytes() const override { return queue_.metadataBytes(); }

 private:
  Queue<bool> queue_;
  // The object the sweep will examine next, or nullptr to start from the
  // tail. Always an object currently in queue_: evict() and onRemove() both
  // move it off an object that is about to leave.
  internal::CacheEntry* hand_ = nullptr;
};

}  // namespace cachesim::policies
