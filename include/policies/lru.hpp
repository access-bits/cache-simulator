#pragma once

#include <string>

#include "internal/datastructures/queue.hpp"
#include "internal/eviction/eviction_policy.hpp"

namespace cachesim::policies {

// Classic LRU on a single Queue: front is most recently used, back is the
// next victim.
class Lru final : public IEvictionPolicy {
 public:
  explicit Lru(const PolicyConfig& config = {}) : queue_(config.entry_hint) {}

  [[nodiscard]] std::string name() const override { return "LRU"; }

  Status onAdmit(internal::CacheEntry* entry, const Request& req) override {
    (void)req;
    return queue_.pushFront(entry);
  }

  Status onHit(internal::CacheEntry* entry, const Request& req) override {
    (void)req;
    return queue_.moveToFront(entry);
  }

  internal::CacheEntry* evict() override { return queue_.popBack(); }

  Status onRemove(internal::CacheEntry* entry) override { return queue_.unlink(entry); }

  Status onResize(internal::CacheEntry* entry, std::uint32_t old_size,
                  std::uint32_t new_size) override {
    (void)entry;
    queue_.adjustBytes(static_cast<std::int64_t>(new_size) - static_cast<std::int64_t>(old_size));
    return Status::kSuccess;
  }

  void clear() override { queue_.clear(); }

  [[nodiscard]] std::uint64_t metadataBytes() const override { return queue_.metadataBytes(); }

 private:
  Queue<> queue_;
};

// FIFO: ordering is insertion order and a hit changes nothing. The honest
// baseline for "does recency information help at all on this trace", and the
// building block S3FIFO and 2Q are made of.
class Fifo final : public IEvictionPolicy {
 public:
  explicit Fifo(const PolicyConfig& config = {}) : queue_(config.entry_hint) {}

  [[nodiscard]] std::string name() const override { return "FIFO"; }

  Status onAdmit(internal::CacheEntry* entry, const Request& req) override {
    (void)req;
    return queue_.pushFront(entry);
  }

  Status onHit(internal::CacheEntry* entry, const Request& req) override {
    (void)entry;
    (void)req;
    return Status::kSuccess;
  }

  internal::CacheEntry* evict() override { return queue_.popBack(); }

  Status onRemove(internal::CacheEntry* entry) override { return queue_.unlink(entry); }

  Status onResize(internal::CacheEntry* entry, std::uint32_t old_size,
                  std::uint32_t new_size) override {
    (void)entry;
    queue_.adjustBytes(static_cast<std::int64_t>(new_size) - static_cast<std::int64_t>(old_size));
    return Status::kSuccess;
  }

  void clear() override { queue_.clear(); }

  [[nodiscard]] std::uint64_t metadataBytes() const override { return queue_.metadataBytes(); }

 private:
  Queue<> queue_;
};

// MRU: evicts the most recently used object. Almost never a good cache, but
// it is the other extreme of the recency axis, and a trace where MRU beats
// LRU is a trace with a scan pattern — which is exactly the thing LRU-based
// policies are blind to, so it is a useful diagnostic to have.
class Mru final : public IEvictionPolicy {
 public:
  explicit Mru(const PolicyConfig& config = {}) : queue_(config.entry_hint) {}

  [[nodiscard]] std::string name() const override { return "MRU"; }

  Status onAdmit(internal::CacheEntry* entry, const Request& req) override {
    (void)req;
    return queue_.pushFront(entry);
  }

  Status onHit(internal::CacheEntry* entry, const Request& req) override {
    (void)req;
    return queue_.moveToFront(entry);
  }

  internal::CacheEntry* evict() override { return queue_.popFront(); }

  Status onRemove(internal::CacheEntry* entry) override { return queue_.unlink(entry); }

  Status onResize(internal::CacheEntry* entry, std::uint32_t old_size,
                  std::uint32_t new_size) override {
    (void)entry;
    queue_.adjustBytes(static_cast<std::int64_t>(new_size) - static_cast<std::int64_t>(old_size));
    return Status::kSuccess;
  }

  void clear() override { queue_.clear(); }

  [[nodiscard]] std::uint64_t metadataBytes() const override { return queue_.metadataBytes(); }

 private:
  Queue<> queue_;
};

}  // namespace cachesim::policies
