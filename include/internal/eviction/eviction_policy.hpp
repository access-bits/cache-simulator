#pragma once

#include <cstdint>
#include <string>

#include "internal/cache/cache_entry.hpp"
#include "internal/cache/cache_structure.hpp"
#include "internal/common/string_util.hpp"
#include "internal/request/request.hpp"
#include "internal/status.hpp"

namespace cachesim {

// Everything a policy needs to size itself, handed over at construction.
//
// Policies get this rather than reaching into the Cache because most of them
// need capacity at *construction* time, not at attach time: ARC sizes its two
// ghost lists to the cache's object capacity, S3FIFO splits capacity 10/90
// between its small and main queues, and a Queue wants its node pool reserved
// before the first push. Passing it in the constructor keeps onAttach for
// what it is actually for — getting at the CacheStructure.
struct PolicyConfig {
  // Byte capacity of the cache this policy will drive.
  std::uint64_t capacity_bytes = 0;

  // Expected peak number of concurrently cached objects. Used only to
  // pre-size pools; a policy must stay correct if the real number exceeds it.
  std::size_t entry_hint = 0;

  // Algorithm-specific options, straight from the config's eviction_params
  // string ("sampling-period=1000,seed=42").
  ParamMap params;

  // Seed for any policy that makes random choices, so a run is reproducible.
  std::uint64_t seed = 0x243f6a8885a308d3ULL;
};

// Runtime-pluggable eviction policy.
//
// A policy works directly with CacheEntry* — it can read identity and size
// and read/write that entry's metadata pointer (normally by handing the entry
// to a Queue or PriorityQueue it owns), but it can never change obj_id or
// size, which stay private to CacheEntry and writable only by CacheStructure.
// That split is what lets a policy be completely wrong without corrupting the
// cache's accounting.
//
// Call order for one request, as driven by Cache::access:
//
//   hit:   onHit(entry, req)
//   miss:  onMiss(req) -> [evict() ...] -> onAdmit(entry, req)
//
// onMiss runs *before* any eviction deliberately: adaptive policies (ARC,
// S3FIFO, 2Q) decide how to adapt from whether the missed id was in a ghost
// list, and that decision changes which list the following evict() should
// take from. A policy that does not adapt can ignore onMiss entirely.
class IEvictionPolicy {
 public:
  virtual ~IEvictionPolicy() = default;

  // Name as it should appear in results. Includes distinguishing parameters
  // where they matter (e.g. "LRU-2", "S3FIFO-small=0.10"), because a sweep
  // over a policy's own parameters otherwise produces rows that cannot be
  // told apart.
  [[nodiscard]] virtual std::string name() const = 0;

  // Called once, after the Cache is constructed and before any request.
  // Override it if you need the CacheStructure itself — a policy that scans
  // all cached objects (clearing access bits, say) needs it; most do not.
  virtual Status onAttach(internal::CacheStructure& structure) {
    (void)structure;
    return Status::kSuccess;
  }

  // A cached object was accessed (cache hit). The entry's size has already
  // been updated if the request carried a new one.
  virtual Status onHit(internal::CacheEntry* entry, const Request& req) = 0;

  // The requested object is not cached. Called before any eviction and
  // before onAdmit, which is what lets an adaptive policy look the id up in
  // its ghost list and adapt first. Default: nothing to do.
  virtual Status onMiss(const Request& req) {
    (void)req;
    return Status::kSuccess;
  }

  // A brand-new object was just admitted. The policy must start tracking it,
  // which is also what sets entry->metadata.
  virtual Status onAdmit(internal::CacheEntry* entry, const Request& req) = 0;

  // Choose a victim, detach it from the policy's own structures (so its
  // metadata is back to nullptr), and return it. Returns nullptr only when
  // the policy genuinely tracks nothing — Cache treats that as fatal, since
  // it only asks when it needs space.
  //
  // Selection and detachment are one call rather than two because the
  // policies that need a scan to pick a victim (Clock, Sieve, Random) mutate
  // state while scanning: Clock advances its hand and clears reference bits
  // on the way. Splitting this into a const "select" plus a separate "remove"
  // would make those policies either lie about constness or scan twice.
  virtual internal::CacheEntry* evict() = 0;

  // A cached object's size changed (a trace re-wrote it at a new size). The
  // cache structure's byte accounting has already been updated; this is the
  // policy's chance to fix up any byte totals of its own, which the queues
  // cannot do themselves because only the policy is told when it happened.
  // Policies that only count objects can ignore it.
  virtual Status onResize(internal::CacheEntry* entry, std::uint32_t old_size,
                          std::uint32_t new_size) {
    (void)entry;
    (void)old_size;
    (void)new_size;
    return Status::kSuccess;
  }

  // Detach an entry that is being removed for a reason other than eviction:
  // an explicit invalidation from a plugin, or an object whose size grew past
  // what the cache can hold. Must leave metadata == nullptr, exactly like
  // evict().
  virtual Status onRemove(internal::CacheEntry* entry) = 0;

  // Drop all policy state. Called when the cache itself is cleared (end of a
  // warm-up phase that is being discarded). A policy whose state is entirely
  // in its queues must empty them here, because every CacheEntry they point
  // at is about to be recycled.
  virtual void clear() {}

  // Bytes of policy-side bookkeeping, for studies that want to charge a
  // policy for its own metadata. Diagnostic only; nothing in the engine
  // changes behaviour based on it.
  [[nodiscard]] virtual std::uint64_t metadataBytes() const { return 0; }
};

}  // namespace cachesim
