#include "internal/cache/cache.hpp"

#include <string>

#include "internal/common/error.hpp"

namespace cachesim {

Cache::Cache(const CacheOptions& options, std::unique_ptr<IEvictionPolicy> policy)
    : options_(options),
      structure_(options.entry_hint, options.max_entries),
      policy_(std::move(policy)) {
  if (policy_ == nullptr) {
    throw EngineError("Cache constructed without an eviction policy");
  }
  if (!ok(policy_->onAttach(structure_))) {
    throw EngineError("IEvictionPolicy::onAttach failed for policy " + policy_->name());
  }
}

void Cache::missPath(const Request& req) {
  if (!ok(policy_->onMiss(req))) failPolicy("onMiss");

  const std::uint32_t effective = effectiveSize(req.size);
  if (effective > options_.capacity_bytes) {
    // Cannot ever be admitted. Counted as a miss (it is one) and reported
    // separately so a run against a too-small capacity is obvious.
    ++stats_.n_oversized;
    return;
  }

  evictToFit(effective);

  internal::CacheEntry* entry = structure_.admit(req.obj_id, effective);
  if (entry == nullptr) {
    throw EngineError(
        "CacheStructure::admit failed: the hard max_entries cap of " +
        std::to_string(structure_.maxEntries()) + " objects was reached");
  }
  if (!ok(policy_->onAdmit(entry, req))) failPolicy("onAdmit");
  ++stats_.n_admit;
}

void Cache::onHitSizeChanged(internal::CacheEntry* entry, std::uint32_t effective) {
  if (effective > options_.capacity_bytes) {
    // The object just grew past the whole cache, so it cannot stay. Drop it
    // explicitly rather than letting the eviction loop find it, which keeps
    // the "cache is never over capacity" invariant true at every exit.
    const std::uint64_t obj_id = entry->objId();
    const std::uint32_t old_size = entry->size();
    if (!ok(policy_->onRemove(entry))) failPolicy("onRemove");
    if (!ok(structure_.erase(entry))) {
      throw EngineError("CacheStructure::erase failed while dropping an oversized object");
    }
    ++stats_.n_evict;
    stats_.n_evict_byte += old_size;
    for (ICacheObserver* observer : observers_) observer->onEvicted(obj_id, old_size);
    return;
  }
  const std::uint32_t old_size = entry->size();
  if (!ok(structure_.resize(entry, effective))) {
    throw EngineError("CacheStructure::resize failed on a cached object");
  }
  if (!ok(policy_->onResize(entry, old_size, effective))) failPolicy("onResize");
  evictToFit(0);
}

void Cache::evictToFit(std::uint64_t extra_bytes) {
  while (structure_.occupiedBytes() + extra_bytes > options_.capacity_bytes) {
    evictOne();
  }
}

void Cache::evictOne() {
  internal::CacheEntry* victim = policy_->evict();
  if (victim == nullptr) {
    throw EngineError(
        "IEvictionPolicy::evict returned no victim for policy " + policy_->name() +
        " while the cache was over capacity (" + std::to_string(structure_.occupiedBytes()) +
        " of " + std::to_string(options_.capacity_bytes) + " bytes, " +
        std::to_string(structure_.objectCount()) +
        " objects) — the policy is not tracking every admitted object");
  }
  const std::uint64_t obj_id = victim->objId();
  const std::uint32_t size = victim->size();
  if (!ok(structure_.erase(victim))) {
    throw EngineError(
        "CacheStructure::erase failed on the victim chosen by " + policy_->name() +
        " — the policy returned an entry it had not detached, or one this cache does not own");
  }
  ++stats_.n_evict;
  stats_.n_evict_byte += size;
  for (ICacheObserver* observer : observers_) observer->onEvicted(obj_id, size);
}

bool Cache::remove(std::uint64_t obj_id) {
  internal::CacheEntry* entry = structure_.find(obj_id);
  if (entry == nullptr) return false;
  const std::uint32_t size = entry->size();
  if (!ok(policy_->onRemove(entry))) failPolicy("onRemove");
  if (!ok(structure_.erase(entry))) {
    throw EngineError("CacheStructure::erase failed during an explicit remove");
  }
  for (ICacheObserver* observer : observers_) observer->onEvicted(obj_id, size);
  return true;
}

void Cache::clear() {
  // The policy first: once the structure recycles its slots, every
  // CacheEntry* the policy holds is pointing at a slot that may be handed to
  // a different object.
  policy_->clear();
  structure_.clear();
}

void Cache::failPolicy(const char* hook) const {
  throw EngineError(std::string("IEvictionPolicy::") + hook + " failed for policy " +
                    policy_->name());
}

}  // namespace cachesim
