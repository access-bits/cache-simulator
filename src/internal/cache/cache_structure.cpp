#include "internal/cache/cache_structure.hpp"

namespace cachesim::internal {

CacheStructure::CacheStructure(std::size_t max_entries) {
  arena_.reserve(1.5*max_entries);
  free_list_.reserve(max_entries);
  index_.reserve(max_entries);
}

CacheEntry* CacheStructure::find(std::uint64_t obj_id) const {
  auto it = index_.find(obj_id);
  return it != index_.end() ? it->second : nullptr;
}

CacheEntry* CacheStructure::admit(std::uint64_t obj_id, std::uint32_t size) {
  CacheEntry* entry = allocateSlot();
  if (entry == nullptr) {
    return nullptr;  // max_entries reached; caller must decide how to handle it
  }
  entry->obj_id_ = obj_id;
  entry->size_ = size;
  entry->metadata = nullptr;
  index_[obj_id] = entry;
  occupied_bytes_ += size;
  return entry;
}

Status CacheStructure::erase(CacheEntry* entry) {
  auto it = index_.find(entry->objId());
  if (it == index_.end() || it->second != entry) {
    return Status::kFailure;
  }
  occupied_bytes_ -= entry->size_;
  index_.erase(it);
  freeSlot(entry);
  return Status::kSuccess;
}

CacheEntry* CacheStructure::allocateSlot() {
  if (!free_list_.empty()) {
    CacheEntry* entry = free_list_.back();
    free_list_.pop_back();
    return entry;
  }
  if (arena_.size() >= arena_.capacity()) {
    // Growing here would reallocate and invalidate every outstanding
    // CacheEntry* (e.g. Queue::metadata) — refuse instead of corrupting them.
    return nullptr;
  }
  arena_.emplace_back();
  return &arena_.back();
}

void CacheStructure::freeSlot(CacheEntry* entry) { free_list_.push_back(entry); }

}  // namespace cachesim::internal
