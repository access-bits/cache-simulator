#pragma once

#include "internal/cache/cache_entry.hpp"
#include "internal/cache/cache_structure.hpp"
#include "internal/status.hpp"

namespace cachesim {

// Runtime-pluggable eviction policy. A policy works directly with
// CacheEntry* — it can read identity/size and read/write its own metadata
// (e.g. via a Queue it owns), but it can never mutate obj_id/size
// themselves; those stay private to CacheEntry, writable only by
// CacheStructure.
class IEvictionPolicy {
 public:
  virtual ~IEvictionPolicy() = default;

  // Optional one-time setup hook, called once right after the Cache is
  // constructed and before any request is processed. Override it if you
  // need something from CacheStructure at setup time (e.g. pre-sizing your
  // own structures against its capacity); default is a no-op.
  virtual void onAttach(internal::CacheStructure& structure) { (void)structure; }

  // A brand-new object was admitted into the cache.
  virtual Status onAdmit(internal::CacheEntry* entry) = 0;

  // An existing object was accessed (cache hit).
  virtual Status onHit(internal::CacheEntry* entry) = 0;

  // Choose the next entry to evict; must not mutate policy state.
  // Returns nullptr if the policy has no candidate to evict.
  virtual internal::CacheEntry* selectVictim() = 0;

  // The entry chosen by selectVictim() is being removed; drop it from
  // whatever internal structure the policy uses to track order.
  virtual Status onEvict(internal::CacheEntry* entry) = 0;
};

}  // namespace cachesim
