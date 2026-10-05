#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

#include "internal/datastructures/priority_queue.hpp"
#include "internal/eviction/eviction_policy.hpp"

namespace cachesim::policies {

// LFU with Dynamic Aging (Arlitt et al.) — LFU's one serious failure mode,
// fixed with a single scalar.
//
// Plain LFU has no way to forget. An object that was requested ten thousand
// times during a burst last week outranks everything admitted since, and it
// sits in the cache until the simulation ends; this is "cache pollution" and
// on any trace with phases it is the dominant error.
//
// LFU-DA gives every object a key K = reference_count + L, where L is a
// global age that is set to the key of each evicted object. L therefore only
// ever rises, and it rises exactly to the level of the least valuable object
// in the cache. A stale object's key is frozen at whatever L was when it was
// last accessed, so as L climbs past it the object becomes evictable without
// anyone having to decay counters or scan the cache.
//
// Implemented as an addressable min-heap keyed on K, with the reference count
// in the node's payload: a hit recomputes K = count + L and re-sifts in
// O(log n).
//
// Ties on K are broken in favour of evicting the object that reached its
// current K first, which is the same FIFO tie-break exact LFU uses. The
// tie-break is not a detail here: every newly admitted object has K = L + 1,
// so on a cold or churning cache most comparisons are ties, and leaving the
// order to whatever the heap happens to do would make the policy
// irreproducible run to run.
class LfuDa final : public IEvictionPolicy {
 public:
  explicit LfuDa(const PolicyConfig& config = {}) : heap_(config.entry_hint) {}

  [[nodiscard]] std::string name() const override { return "LFU-DA"; }

  Status onAdmit(internal::CacheEntry* entry, const Request& req) override {
    (void)req;
    const Status status = heap_.insert(entry, Key{age_ + 1, ++clock_});
    if (!ok(status)) return status;
    heap_.payload(entry) = 1;
    return Status::kSuccess;
  }

  Status onHit(internal::CacheEntry* entry, const Request& req) override {
    (void)req;
    if (!heap_.contains(entry)) return Status::kFailure;
    const std::int64_t count = ++heap_.payload(entry);
    return heap_.updatePriority(entry, Key{age_ + count, ++clock_});
  }

  internal::CacheEntry* evict() override {
    internal::CacheEntry* victim = heap_.peek();
    if (victim == nullptr) return nullptr;
    // The age rises to the departing object's key. Everything still cached
    // has a key at least this high, so no object is ever "aged past" while it
    // is still the most valuable thing present.
    age_ = heap_.priorityOf(victim).first;
    return heap_.pop();
  }

  Status onRemove(internal::CacheEntry* entry) override { return heap_.remove(entry); }

  Status onResize(internal::CacheEntry* entry, std::uint32_t old_size,
                  std::uint32_t new_size) override {
    (void)entry;
    heap_.adjustBytes(static_cast<std::int64_t>(new_size) - static_cast<std::int64_t>(old_size));
    return Status::kSuccess;
  }

  void clear() override {
    heap_.clear();
    age_ = 0;
    clock_ = 0;
  }

  [[nodiscard]] std::uint64_t metadataBytes() const override { return heap_.metadataBytes(); }

 private:
  // (K, tick): K is count + age, tick is when the object reached that K.
  using Key = std::pair<std::int64_t, std::int64_t>;

  // Min-heap: std::greater makes the root the smallest key, which is the
  // least valuable object. The payload is the object's reference count.
  PriorityQueue<Key, std::int64_t, std::greater<Key>> heap_;
  std::int64_t age_ = 0;
  std::int64_t clock_ = 0;
};

}  // namespace cachesim::policies
