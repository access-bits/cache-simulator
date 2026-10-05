#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "ankerl/unordered_dense.h"
#include "internal/cache/cache_entry.hpp"
#include "internal/status.hpp"

namespace cachesim::internal {

// Owns the pool of cache entries and the obj_id -> entry lookup. Purely
// "what is cached and where do I find it" — no notion of ordering at all;
// that lives in whichever data structure uses CacheEntry::metadata.
//
// Storage is a std::vector reserved once to max_entries and never grown
// past it: CacheEntry addresses are handed out and stashed inside metadata
// pointers elsewhere (e.g. Queue), so they must never move. A vector only
// invalidates existing elements' addresses when it reallocates, which
// reserve() + never exceeding that capacity rules out entirely — and it's
// more cache-friendly than a chunked container like std::deque. The
// trade-off: max_entries is a hard cap, enforced by admit() returning
// nullptr once it's reached (see Cache, which treats that as fatal).
class CacheStructure {
 public:
  // max_entries: the maximum number of distinct objects this cache will
  // ever hold concurrently. Must be sized by the caller (e.g. capacity in
  // bytes divided by the smallest expected object size) — there is no safe
  // way to grow past it later without invalidating outstanding pointers.
  explicit CacheStructure(std::size_t max_entries);

  // Returns nullptr if obj_id is not currently cached.
  CacheEntry* find(std::uint64_t obj_id) const;
  bool contains(std::uint64_t obj_id) const { return find(obj_id) != nullptr; }

  // Adds a brand-new object; caller must have already checked find(obj_id) == nullptr.
  // Returns nullptr if max_entries has been reached (arena exhausted).
  CacheEntry* admit(std::uint64_t obj_id, std::uint32_t size);

  // Frees the entry back to the pool. Caller must have already cleared
  // metadata (unlinked it from whatever was tracking it) first. Fails if
  // entry isn't currently a live, indexed entry of this CacheStructure.
  Status erase(CacheEntry* entry);

  std::uint64_t occupiedBytes() const { return occupied_bytes_; }
  std::size_t objectCount() const { return index_.size(); }

 private:
  CacheEntry* allocateSlot();
  void freeSlot(CacheEntry* entry);

  std::vector<CacheEntry> arena_;
  std::vector<CacheEntry*> free_list_;
  ankerl::unordered_dense::map<std::uint64_t, CacheEntry*> index_;
  std::uint64_t occupied_bytes_ = 0;
};

}  // namespace cachesim::internal
