#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "internal/cache/cache_structure.hpp"
#include "internal/eviction/eviction_policy.hpp"
#include "internal/request/request.hpp"

namespace cachesim {

// A single-threaded, size-based cache: one instance == one (policy, capacity)
// configuration. Parameter sweeps run many independent instances in parallel,
// each on its own thread, so there is no synchronization inside this class.
class Cache {
 public:
  // max_entries: the maximum number of distinct objects this cache will
  // ever hold concurrently — see CacheStructure for why this must be sized
  // by the caller rather than guessed from capacity_bytes alone.
  Cache(std::uint64_t capacity_bytes, std::unique_ptr<IEvictionPolicy> policy,
        std::size_t max_entries);

  // Replays one request against the cache. Returns true on hit, false on miss.
  // Throws std::runtime_error if the policy or CacheStructure report an
  // internal failure (e.g. max_entries exhausted) — these represent
  // misconfiguration or a policy bug, not an ordinary cache miss.
  bool access(const Request& req);

  std::uint64_t occupiedBytes() const { return structure_.occupiedBytes(); }
  std::uint64_t capacityBytes() const { return capacity_bytes_; }
  std::size_t objectCount() const { return structure_.objectCount(); }

 private:
  void evict(internal::CacheEntry* victim);

  std::uint64_t capacity_bytes_;
  internal::CacheStructure structure_;
  std::unique_ptr<IEvictionPolicy> policy_;
};

}  // namespace cachesim
