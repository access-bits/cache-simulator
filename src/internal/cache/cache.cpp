#include "internal/cache/cache.hpp"

#include <stdexcept>
#include <utility>

namespace cachesim {

Cache::Cache(std::uint64_t capacity_bytes, std::unique_ptr<IEvictionPolicy> policy,
             std::size_t max_entries)
    : capacity_bytes_(capacity_bytes), structure_(max_entries), policy_(std::move(policy)) {
  policy_->onAttach(structure_);
}

bool Cache::access(const Request& req) {
  if (auto* entry = structure_.find(req.obj_id)) {
    if (policy_->onHit(entry) != Status::kSuccess) {
      throw std::runtime_error("IEvictionPolicy::onHit failed");
    }
    return true;
  }

  if (req.size > capacity_bytes_) {
    // Larger than the whole cache; can never be admitted.
    return false;
  }

  while (structure_.occupiedBytes() + req.size > capacity_bytes_) {
    evict(policy_->selectVictim());
  }

  auto* entry = structure_.admit(req.obj_id, req.size);
  if (entry == nullptr) {
    throw std::runtime_error("CacheStructure::admit failed: max_entries exhausted");
  }
  if (policy_->onAdmit(entry) != Status::kSuccess) {
    throw std::runtime_error("IEvictionPolicy::onAdmit failed");
  }

  return false;
}

void Cache::evict(internal::CacheEntry* victim) {
  if (victim == nullptr) {
    throw std::runtime_error("IEvictionPolicy::selectVictim returned no candidate to evict");
  }
  if (policy_->onEvict(victim) != Status::kSuccess) {
    throw std::runtime_error("IEvictionPolicy::onEvict failed");
  }
  if (structure_.erase(victim) != Status::kSuccess) {
    throw std::runtime_error("CacheStructure::erase failed on the evicted entry");
  }
}

}  // namespace cachesim
