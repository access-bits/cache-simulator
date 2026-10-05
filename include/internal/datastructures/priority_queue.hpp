#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "internal/cache/cache_entry.hpp"
#include "internal/common/compiler.hpp"
#include "internal/datastructures/block_pool.hpp"
#include "internal/datastructures/metadata.hpp"
#include "internal/datastructures/queue.hpp"
#include "internal/status.hpp"

namespace cachesim {

// Intrusive, addressable binary heap over cached objects — the same idea as
// Queue, but answering "give me the extreme object by some key" instead of
// "give me the end of an ordering".
//
// "Addressable" is the whole point: CacheEntry::metadata points straight at
// the object's heap node, so when the object is accessed again its key can be
// changed in place with an O(log n) re-sift, rather than an O(n) search for
// where in the heap it currently sits. Belady is the motivating case — every
// hit revises that object's next-access time — and it is unusable without
// this property.
//
// By default the root is the element that compares greatest under Compare
// (std::less gives a max-heap, matching std::priority_queue). Belady wants a
// max-heap keyed on next_access_vtime: the object used furthest in the future
// is the best victim, and "never again" is kNeverAgain == INT64_MAX, which
// sorts above every real time without a special case.
//
// Payload: extra per-object state carried alongside the node, same role as
// Queue's Payload.
template <typename Priority = std::int64_t, typename Payload = NoData,
          typename Compare = std::less<Priority>>
class PriorityQueue {
 public:
  explicit PriorityQueue(std::size_t reserve_hint = 0, Compare compare = Compare{})
      : pool_(reserve_hint), compare_(std::move(compare)) {
    if (reserve_hint > 0) heap_.reserve(reserve_hint);
  }

  PriorityQueue(const PriorityQueue&) = delete;
  PriorityQueue& operator=(const PriorityQueue&) = delete;
  // Not movable, for the same reason as Queue: nodes record their owner's
  // address.
  PriorityQueue(PriorityQueue&&) = delete;
  PriorityQueue& operator=(PriorityQueue&&) = delete;

  // Fails if entry is null or already tracked by some queue or heap.
  Status insert(internal::CacheEntry* entry, Priority priority) {
    if (CACHESIM_UNLIKELY(entry == nullptr || entry->metadata != nullptr)) {
      return Status::kFailure;
    }
    Node* node = pool_.allocate();
    node->owner = this;
    node->cache_entry = entry;
    node->priority = std::move(priority);
    node->payload = Payload{};
    node->heap_pos = heap_.size();
    entry->metadata = node;
    heap_.push_back(node);
    bytes_ += entry->size();
    siftUp(node->heap_pos);
    return Status::kSuccess;
  }

  // Repositions entry after its key changed. O(log n).
  Status updatePriority(internal::CacheEntry* entry, Priority priority) {
    Node* node = nodeOf(entry);
    if (CACHESIM_UNLIKELY(node == nullptr)) return Status::kFailure;
    const bool increased = compare_(node->priority, priority);
    const bool decreased = compare_(priority, node->priority);
    node->priority = std::move(priority);
    if (increased) {
      siftUp(node->heap_pos);
    } else if (decreased) {
      siftDown(node->heap_pos);
    }
    return Status::kSuccess;
  }

  // Root entry without removing it; nullptr if empty.
  [[nodiscard]] internal::CacheEntry* peek() const {
    return heap_.empty() ? nullptr : heap_.front()->cache_entry;
  }

  // Removes and returns the root, already detached; nullptr if empty.
  internal::CacheEntry* pop() {
    if (heap_.empty()) return nullptr;
    internal::CacheEntry* result = heap_.front()->cache_entry;
    removeAt(0);
    return result;
  }

  // Removes an arbitrary tracked entry, not necessarily the root.
  Status remove(internal::CacheEntry* entry) {
    Node* node = nodeOf(entry);
    if (CACHESIM_UNLIKELY(node == nullptr)) return Status::kFailure;
    removeAt(node->heap_pos);
    return Status::kSuccess;
  }

  [[nodiscard]] CACHESIM_ALWAYS_INLINE bool contains(
      const internal::CacheEntry* entry) const {
    return entry != nullptr && entry->metadata != nullptr && entry->metadata->owner == this;
  }

  // Precondition: contains(entry).
  [[nodiscard]] const Priority& priorityOf(const internal::CacheEntry* entry) const {
    return static_cast<const Node*>(entry->metadata)->priority;
  }
  [[nodiscard]] Payload& payload(internal::CacheEntry* entry) {
    return static_cast<Node*>(entry->metadata)->payload;
  }
  [[nodiscard]] const Payload& payload(const internal::CacheEntry* entry) const {
    return static_cast<const Node*>(entry->metadata)->payload;
  }

  [[nodiscard]] bool empty() const { return heap_.empty(); }
  [[nodiscard]] std::size_t size() const { return heap_.size(); }
  [[nodiscard]] std::uint64_t bytes() const { return bytes_; }

  void adjustBytes(std::int64_t delta) {
    bytes_ = static_cast<std::uint64_t>(static_cast<std::int64_t>(bytes_) + delta);
  }

  void clear() {
    for (Node* node : heap_) {
      node->cache_entry->metadata = nullptr;
      node->owner = nullptr;
    }
    heap_.clear();
    bytes_ = 0;
    pool_.releaseAll();
  }

  [[nodiscard]] std::uint64_t metadataBytes() const {
    return pool_.memoryBytes() + static_cast<std::uint64_t>(heap_.capacity()) * sizeof(Node*);
  }

 private:
  struct Node : internal::MetadataNode {
    internal::CacheEntry* cache_entry = nullptr;
    Priority priority{};
    std::size_t heap_pos = 0;
    [[no_unique_address]] Payload payload{};
  };

  Node* nodeOf(internal::CacheEntry* entry) const {
    if (entry == nullptr || entry->metadata == nullptr || entry->metadata->owner != this) {
      return nullptr;
    }
    return static_cast<Node*>(entry->metadata);
  }

  void place(std::size_t index, Node* node) {
    heap_[index] = node;
    node->heap_pos = index;
  }

  // Sifts by moving a hole rather than swapping pairs: one write per level
  // instead of three, which matters because every Belady hit does a full
  // sift.
  void siftUp(std::size_t index) {
    Node* node = heap_[index];
    while (index > 0) {
      const std::size_t parent = (index - 1) / 2;
      if (!compare_(heap_[parent]->priority, node->priority)) break;
      place(index, heap_[parent]);
      index = parent;
    }
    place(index, node);
  }

  void siftDown(std::size_t index) {
    Node* node = heap_[index];
    const std::size_t n = heap_.size();
    for (;;) {
      const std::size_t left = 2 * index + 1;
      if (left >= n) break;
      const std::size_t right = left + 1;
      std::size_t child = left;
      if (right < n && compare_(heap_[left]->priority, heap_[right]->priority)) {
        child = right;
      }
      if (!compare_(node->priority, heap_[child]->priority)) break;
      place(index, heap_[child]);
      index = child;
    }
    place(index, node);
  }

  void removeAt(std::size_t index) {
    Node* node = heap_[index];
    bytes_ -= node->cache_entry->size();
    node->cache_entry->metadata = nullptr;
    node->owner = nullptr;

    const std::size_t last = heap_.size() - 1;
    if (index != last) {
      place(index, heap_[last]);
      heap_.pop_back();
      // The element moved into the hole came from the bottom of the heap, so
      // it can need to travel either way depending on where the hole was.
      // Exactly one of these does anything.
      siftDown(index);
      siftUp(index);
    } else {
      heap_.pop_back();
    }
    pool_.release(node);
  }

  internal::BlockPool<Node> pool_;
  std::vector<Node*> heap_;  // array heap of pointers into the pool
  std::uint64_t bytes_ = 0;
  [[no_unique_address]] Compare compare_;
};

}  // namespace cachesim
