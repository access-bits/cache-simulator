#pragma once

#include "internal/datastructures/queue.hpp"
#include "internal/eviction/eviction_policy.hpp"

namespace cachesim {

// Classic LRU, built on a single Queue: head = most recently used,
// tail = next victim.
class LRUPolicy final : public IEvictionPolicy {
 public:
  explicit LRUPolicy(std::size_t reserve_hint = 0) : queue_(reserve_hint) {}

  Status onAdmit(internal::CacheEntry* entry) override { return queue_.pushFront(entry); }
  Status onHit(internal::CacheEntry* entry) override { return queue_.moveToFront(entry); }
  internal::CacheEntry* selectVictim() override { return queue_.back(); }
  Status onEvict(internal::CacheEntry* entry) override { return queue_.unlink(entry); }

 private:
  Queue<> queue_;
};

}  // namespace cachesim
