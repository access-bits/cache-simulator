#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "internal/cache/cache_structure.hpp"
#include "internal/cache/stats.hpp"
#include "internal/common/compiler.hpp"
#include "internal/eviction/eviction_policy.hpp"
#include "internal/request/request.hpp"

namespace cachesim {

// Notified when the cache removes an object. Exists for layers that model
// something outside the cache which has to stay coherent with it — the
// per-CPU TLB filter has to invalidate its entry for an object the shared
// cache just dropped, or it would keep reporting hits on an object that is no
// longer there.
//
// Kept separate from IEvictionPolicy because an observer must not be able to
// influence the decision, only learn about it.
class ICacheObserver {
 public:
  virtual ~ICacheObserver() = default;
  virtual void onEvicted(std::uint64_t obj_id, std::uint32_t size) = 0;
};

struct CacheOptions {
  // Byte capacity. With object-count capacity (every object size 1) this is
  // simply the number of objects.
  std::uint64_t capacity_bytes = 0;

  // Expected peak object count, used to size the arena and the index in one
  // allocation. When object sizes are uniform this is exact; otherwise a
  // rough guess is fine, since the arena grows rather than failing.
  std::size_t entry_hint = 0;

  // Hard cap on resident objects; 0 means no cap.
  std::size_t max_entries = 0;

  // Per-object bookkeeping charged against capacity, so a study can account
  // for the fact that caching an object costs more than the object's bytes
  // (libCacheSim's consider_obj_metadata). Charged to occupancy, not to the
  // request byte counts, so the byte miss ratio still refers to real traffic.
  std::uint32_t obj_metadata_size = 0;

  // Whether a request that names a different size for an already-cached
  // object updates that object's size. True matches real caches and
  // libCacheSim; false pins each object at the size it was first seen with,
  // which some studies prefer because it keeps occupancy monotone in the
  // trace's unique-byte footprint.
  bool update_size_on_hit = true;
};

// A single-threaded, size-based cache: one instance is one (policy, capacity)
// configuration. Parameter sweeps run many independent instances in parallel,
// each on its own thread with its own memory, so there is no synchronization
// anywhere in this class — that is the point of it, and the reason a sweep
// here scales linearly where libCacheSim's shared batch queue does not.
class Cache {
 public:
  Cache(const CacheOptions& options, std::unique_ptr<IEvictionPolicy> policy);

  Cache(const Cache&) = delete;
  Cache& operator=(const Cache&) = delete;

  // Replays one request. Returns true on hit, false on miss.
  //
  // Defined inline because the replay loop calls nothing else: with the body
  // visible, the compiler keeps the structure's index pointer and the stats
  // counters in registers across requests and folds the hit path down to a
  // hash probe plus one indirect call into the policy. The cold paths
  // (eviction, oversized objects, errors) are out-of-line, so the inlined
  // footprint stays small enough to be worth inlining.
  CACHESIM_HOT bool access(const Request& req) {
    ++stats_.n_req;
    stats_.n_req_byte += req.size;

    if (internal::CacheEntry* entry = structure_.find(req.obj_id)) {
      ++stats_.n_hit;
      stats_.n_hit_byte += req.size;
      const std::uint32_t effective = effectiveSize(req.size);
      if (CACHESIM_UNLIKELY(options_.update_size_on_hit && entry->size() != effective)) {
        // Order matters: let the policy see the hit while the entry is
        // certainly still alive, because growing it may evict it.
        if (CACHESIM_UNLIKELY(!ok(policy_->onHit(entry, req)))) failPolicy("onHit");
        onHitSizeChanged(entry, effective);
        return true;
      }
      if (CACHESIM_UNLIKELY(!ok(policy_->onHit(entry, req)))) failPolicy("onHit");
      return true;
    }

    missPath(req);
    return false;
  }

  // Replays a run of consecutive requests.
  //
  // Semantically identical to calling access() on each in turn -- the cache
  // is stateful and order-dependent, so the work cannot be reordered or
  // overlapped. What this adds is prefetching: while request i is being
  // handled, the index slot for request i + kPrefetchDistance is pulled into
  // cache.
  //
  // That is worth doing because this simulator is memory-latency-bound on any
  // interesting trace, not instruction-bound. Measured on a 100M-request Zipf
  // trace, LRU runs at 26 M req/s with a 1,000-object cache and 6.5 M req/s
  // with a 1,000,000-object cache, on identical code -- the difference is
  // entirely that the index stopped fitting in L3. Each request's lookup then
  // stalls on a memory access that could have been started much earlier,
  // because the one thing we *do* know ahead of time is which object each
  // upcoming request names.
  //
  // A prefetch is a hint with no semantic effect: it cannot fault and it is
  // harmless if the object turns out not to be cached, or is evicted before
  // its request arrives. So this stays exactly as correct as the loop it
  // replaces, which is why `hits` (if given) is filled with the same answers
  // access() would have returned.
  // kIndexDistance: how far ahead to fetch the index group for a request.
  // kEntryDistance: how far ahead to resolve a request and fetch the entry
  //   and policy node it names. Shorter, because that stage wants the group
  //   it probes to have arrived already -- which is what the first stage,
  //   running further ahead, is for.
  template <std::size_t kIndexDistance = 16, std::size_t kEntryDistance = 5>
  CACHESIM_HOT void accessBatch(const Request* requests, std::size_t count,
                                bool* hits = nullptr) {
    // Both stages are decided once per batch and then baked into the loop as
    // template arguments, so a cache that does not want them runs a loop body
    // with nothing extra in it at all -- not even a branch. Neither stage is
    // free: the first is an instruction and a hash, the second a whole
    // speculative lookup, and on an index that already fits in cache both are
    // pure overhead.
    if (!structure_.prefetchWorthwhile()) {
      accessRun<false, false, kIndexDistance, kEntryDistance>(requests, count, hits);
    } else if (!structure_.deepPrefetchWorthwhile()) {
      accessRun<true, false, kIndexDistance, kEntryDistance>(requests, count, hits);
    } else {
      accessRun<true, true, kIndexDistance, kEntryDistance>(requests, count, hits);
    }
  }

  // Removes an object from the cache if present, as an explicit
  // invalidation rather than an eviction. Returns true if it was cached.
  bool remove(std::uint64_t obj_id);

  // Drops every cached object and all policy state, keeping the allocated
  // arena. Stats are left alone — resetStats() is separate, because
  // discarding a warm-up phase means keeping the cache contents and zeroing
  // the counters, which is the opposite combination.
  void clear();

  void resetStats() { stats_.reset(); }

  void addObserver(ICacheObserver* observer) { observers_.push_back(observer); }

  [[nodiscard]] const Stats& stats() const { return stats_; }
  [[nodiscard]] Stats& mutableStats() { return stats_; }
  [[nodiscard]] std::uint64_t occupiedBytes() const { return structure_.occupiedBytes(); }
  [[nodiscard]] std::uint64_t capacityBytes() const { return options_.capacity_bytes; }
  [[nodiscard]] std::size_t objectCount() const { return structure_.objectCount(); }
  [[nodiscard]] std::string policyName() const { return policy_->name(); }
  [[nodiscard]] const IEvictionPolicy& policy() const { return *policy_; }
  [[nodiscard]] IEvictionPolicy& mutablePolicy() { return *policy_; }
  [[nodiscard]] const internal::CacheStructure& structure() const { return structure_; }

 private:
  template <bool kIndexPrefetch, bool kEntryPrefetch, std::size_t kIndexDistance,
            std::size_t kEntryDistance>
  CACHESIM_HOT void accessRun(const Request* requests, std::size_t count, bool* hits) {
    if constexpr (kIndexPrefetch) {
      // Fill the pipeline before the loop starts, so the first requests are
      // not the only ones that pay full latency.
      for (std::size_t i = 0; i < std::min(kIndexDistance, count); ++i) {
        structure_.prefetch(requests[i].obj_id);
      }
    }
    for (std::size_t i = 0; i < count; ++i) {
      if constexpr (kIndexPrefetch) {
        if (i + kIndexDistance < count) {
          structure_.prefetch(requests[i + kIndexDistance].obj_id);
        }
      }
      if constexpr (kEntryPrefetch) {
        if (i + kEntryDistance < count) {
          structure_.prefetchEntry(requests[i + kEntryDistance].obj_id);
        }
      }
      const bool hit = access(requests[i]);
      if (hits != nullptr) hits[i] = hit;
    }
  }

  [[nodiscard]] std::uint32_t effectiveSize(std::uint32_t request_size) const {
    return request_size + options_.obj_metadata_size;
  }

  // Everything that happens on a miss. Out-of-line: it is the branch that
  // does real work (adapt, evict, admit), and keeping it out of access()
  // keeps the hit path tight.
  CACHESIM_NOINLINE void missPath(const Request& req);

  // An object that is already cached was requested at a new size.
  CACHESIM_NOINLINE void onHitSizeChanged(internal::CacheEntry* entry, std::uint32_t effective);

  // Evicts until `extra_bytes` more would still fit. Throws if the policy
  // cannot produce a victim while the cache is over capacity.
  void evictToFit(std::uint64_t extra_bytes);

  // Evicts exactly one object. Separated out so both eviction paths share
  // the policy/structure/observer sequencing, which is easy to get subtly
  // wrong (the entry must be detached from the policy before the structure
  // recycles its slot, and its id must be read before either).
  void evictOne();

  [[noreturn]] void failPolicy(const char* hook) const;

  CacheOptions options_;
  internal::CacheStructure structure_;
  std::unique_ptr<IEvictionPolicy> policy_;
  Stats stats_;
  // Empty in every run that does not use plugins, so the cost is one branch
  // on a vector size that is already hot.
  std::vector<ICacheObserver*> observers_;
};

}  // namespace cachesim
