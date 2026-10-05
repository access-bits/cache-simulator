#pragma once

#include <cstdint>

#include "internal/datastructures/metadata.hpp"

namespace cachesim::internal {

class CacheStructure;

// The arena's per-object record: identity, size, and one pointer that
// whichever data structure currently orders this object uses to find its own
// bookkeeping (a Queue node, a heap node, ...).
//
// Identity and size are private and writable only by CacheStructure, which
// owns the obj_id -> entry index and the byte accounting; letting a policy
// change either behind CacheStructure's back would desynchronize both.
// `metadata` is public and fair game for whichever structure owns the entry's
// ordering — that is the whole point of it.
//
// 24 bytes, so three entries fill two cache lines and a scan over the arena
// (which Clock, Sieve and Random all do, in one form or another) streams.
class CacheEntry {
 public:
  MetadataNode* metadata = nullptr;

  [[nodiscard]] std::uint64_t objId() const { return obj_id_; }
  [[nodiscard]] std::uint32_t size() const { return size_; }

 private:
  friend class CacheStructure;

  std::uint64_t obj_id_ = 0;
  std::uint32_t size_ = 0;
};

static_assert(sizeof(CacheEntry) == 24, "CacheEntry should stay 24 bytes");

}  // namespace cachesim::internal
