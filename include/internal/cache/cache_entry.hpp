#pragma once

#include <cstdint>

namespace cachesim::internal {

class CacheStructure;

// The arena's actual per-object storage: identity, size, and an opaque
// pointer any single tracking data structure (e.g. Queue) can use to stash
// its own per-object bookkeeping (e.g. a QueueEntry with prev/next links).
// Only CacheStructure may set identity/size; metadata is fair game for
// whichever data structure currently owns this entry's ordering.
class CacheEntry {
 public:
  void* metadata = nullptr;

  std::uint64_t objId() const { return obj_id_; }
  std::uint32_t size() const { return size_; }

 private:
  friend class CacheStructure;

  std::uint64_t obj_id_ = 0;
  std::uint32_t size_ = 0;
};

}  // namespace cachesim::internal
