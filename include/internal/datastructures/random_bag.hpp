#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "internal/cache/cache_entry.hpp"
#include "internal/common/compiler.hpp"
#include "internal/common/hash.hpp"
#include "internal/datastructures/block_pool.hpp"
#include "internal/datastructures/metadata.hpp"
#include "internal/datastructures/queue.hpp"
#include "internal/status.hpp"

namespace cachesim {

// An unordered set of cached objects supporting uniform random selection, all
// in O(1): insert, remove a specific entry, and sample.
//
// The classic "swap with the last element" trick, made addressable the same
// way Queue and PriorityQueue are: a dense vector holds the members, and each
// member's node records its own index in that vector, so removing an
// arbitrary entry is a swap and two index writes instead of a search.
//
// Random eviction is worth having as more than a curiosity. It is the
// baseline that says how much of a policy's hit ratio is really coming from
// its ordering, and it is the sampling primitive that size-aware and
// hyperbolic-style policies are built on ("draw k candidates, evict the worst
// of them").
template <typename Payload = NoData>
class RandomBag {
 public:
  explicit RandomBag(std::size_t reserve_hint = 0,
                     std::uint64_t seed = 0x243f6a8885a308d3ULL)
      : pool_(reserve_hint), rng_(seed) {
    if (reserve_hint > 0) members_.reserve(reserve_hint);
  }

  RandomBag(const RandomBag&) = delete;
  RandomBag& operator=(const RandomBag&) = delete;
  RandomBag(RandomBag&&) = delete;
  RandomBag& operator=(RandomBag&&) = delete;

  Status insert(internal::CacheEntry* entry) {
    if (CACHESIM_UNLIKELY(entry == nullptr || entry->metadata != nullptr)) {
      return Status::kFailure;
    }
    Node* node = pool_.allocate();
    node->owner = this;
    node->cache_entry = entry;
    node->position = members_.size();
    node->payload = Payload{};
    entry->metadata = node;
    members_.push_back(node);
    bytes_ += entry->size();
    return Status::kSuccess;
  }

  Status remove(internal::CacheEntry* entry) {
    Node* node = nodeOf(entry);
    if (CACHESIM_UNLIKELY(node == nullptr)) return Status::kFailure;
    removeNode(node);
    return Status::kSuccess;
  }

  // Removes and returns a uniformly chosen member, already detached.
  // nullptr if empty.
  internal::CacheEntry* popRandom() {
    if (members_.empty()) return nullptr;
    Node* node = members_[rng_.below(members_.size())];
    internal::CacheEntry* entry = node->cache_entry;
    removeNode(node);
    return entry;
  }

  // A uniformly chosen member without removing it; nullptr if empty.
  [[nodiscard]] internal::CacheEntry* sample() {
    if (members_.empty()) return nullptr;
    return members_[rng_.below(members_.size())]->cache_entry;
  }

  [[nodiscard]] CACHESIM_ALWAYS_INLINE bool contains(
      const internal::CacheEntry* entry) const {
    return entry != nullptr && entry->metadata != nullptr && entry->metadata->owner == this;
  }

  // Precondition: contains(entry).
  [[nodiscard]] Payload& payload(internal::CacheEntry* entry) {
    return static_cast<Node*>(entry->metadata)->payload;
  }
  [[nodiscard]] const Payload& payload(const internal::CacheEntry* entry) const {
    return static_cast<const Node*>(entry->metadata)->payload;
  }

  // Draws `count` distinct members (or every member, if there are fewer) and
  // hands each to `fn`. The basis of sampled eviction: a policy whose
  // victim score depends on the current time cannot keep a heap, but it can
  // look at a few dozen random objects and take the worst, which for any
  // reasonable score gets within a hair of the true worst.
  template <typename Fn>
  void sampleSome(std::size_t count, Fn&& fn) {
    const std::size_t n = members_.size();
    if (n == 0) return;
    if (count >= n) {
      for (Node* node : members_) fn(node->cache_entry, node->payload);
      return;
    }
    // Partial Fisher-Yates over the dense array: draws without replacement,
    // and leaves the array a valid permutation of itself, so no bookkeeping
    // is needed afterwards beyond fixing the positions we touched.
    for (std::size_t i = 0; i < count; ++i) {
      const std::size_t j = i + static_cast<std::size_t>(rng_.below(n - i));
      Node* a = members_[i];
      Node* b = members_[j];
      members_[i] = b;
      members_[j] = a;
      b->position = i;
      a->position = j;
      fn(b->cache_entry, b->payload);
    }
  }

  [[nodiscard]] bool empty() const { return members_.empty(); }
  [[nodiscard]] std::size_t size() const { return members_.size(); }
  [[nodiscard]] std::uint64_t bytes() const { return bytes_; }
  void adjustBytes(std::int64_t delta) {
    bytes_ = static_cast<std::uint64_t>(static_cast<std::int64_t>(bytes_) + delta);
  }

  void clear() {
    for (Node* node : members_) {
      node->cache_entry->metadata = nullptr;
      node->owner = nullptr;
    }
    members_.clear();
    bytes_ = 0;
    pool_.releaseAll();
  }

  [[nodiscard]] std::uint64_t metadataBytes() const {
    return pool_.memoryBytes() + static_cast<std::uint64_t>(members_.capacity()) * sizeof(Node*);
  }

 private:
  struct Node : internal::MetadataNode {
    internal::CacheEntry* cache_entry = nullptr;
    std::size_t position = 0;
    [[no_unique_address]] Payload payload{};
  };

  Node* nodeOf(internal::CacheEntry* entry) const {
    if (entry == nullptr || entry->metadata == nullptr || entry->metadata->owner != this) {
      return nullptr;
    }
    return static_cast<Node*>(entry->metadata);
  }

  void removeNode(Node* node) {
    const std::size_t position = node->position;
    Node* last = members_.back();
    members_[position] = last;
    last->position = position;
    members_.pop_back();
    bytes_ -= node->cache_entry->size();
    node->cache_entry->metadata = nullptr;
    node->owner = nullptr;
    pool_.release(node);
  }

  internal::BlockPool<Node> pool_;
  std::vector<Node*> members_;
  std::uint64_t bytes_ = 0;
  Rng rng_;
};

}  // namespace cachesim
