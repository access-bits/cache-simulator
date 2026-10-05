#pragma once

#include <string>

#include "internal/datastructures/random_bag.hpp"
#include "internal/eviction/eviction_policy.hpp"

namespace cachesim::policies {

// Uniformly random eviction, in O(1) per operation.
//
// Worth having for two reasons. It is the floor that tells you how much of a
// policy's hit ratio comes from its ordering rather than from the cache
// simply being large enough; and on traces with no reuse structure it is
// often within a point or two of LRU, which is useful to know before
// attributing a result to a clever policy.
class Random final : public IEvictionPolicy {
 public:
  explicit Random(const PolicyConfig& config = {}) : bag_(config.entry_hint, config.seed) {}

  [[nodiscard]] std::string name() const override { return "Random"; }

  Status onAdmit(internal::CacheEntry* entry, const Request& req) override {
    (void)req;
    return bag_.insert(entry);
  }

  Status onHit(internal::CacheEntry* entry, const Request& req) override {
    (void)entry;
    (void)req;
    return Status::kSuccess;
  }

  internal::CacheEntry* evict() override { return bag_.popRandom(); }

  Status onRemove(internal::CacheEntry* entry) override { return bag_.remove(entry); }

  Status onResize(internal::CacheEntry* entry, std::uint32_t old_size,
                  std::uint32_t new_size) override {
    (void)entry;
    bag_.adjustBytes(static_cast<std::int64_t>(new_size) - static_cast<std::int64_t>(old_size));
    return Status::kSuccess;
  }

  void clear() override { bag_.clear(); }

  [[nodiscard]] std::uint64_t metadataBytes() const override { return bag_.metadataBytes(); }

 private:
  RandomBag<> bag_;
};

}  // namespace cachesim::policies
