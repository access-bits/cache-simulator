#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

#include "internal/cache/cache_entry.hpp"
#include "internal/datastructures/queue.hpp"
#include "internal/status.hpp"

namespace cachesim {

// Intrusive, addressable max-heap over cached objects — same idea as
// Queue, but for "give me the highest-priority object" instead of
// FIFO/LRU ordering (e.g. Belady: priority = next_access_vtime, so the
// object accessed furthest in the future is always at the top).
//
// "Addressable" is the key property: CacheEntry::metadata points directly
// at this object's heap node, so its priority can be changed in place
// (O(log n) re-sift) without searching the heap for it — the same
// back-pointer trick Queue uses, applied to a heap instead of a list.
//
// Payload: extra per-object state carried alongside the node, same role
// as Queue's Payload (e.g. a policy might want to remember an object's
// insertion time in addition to its priority).
//
// Header-only for the same reason as Queue: template methods must be
// visible wherever they're instantiated.
template <typename Priority = std::int64_t, typename Payload = NoData>
class PriorityQueue {
 public:
  explicit PriorityQueue(std::size_t reserve_hint = 0) {
    if (reserve_hint > 0) {
      heap_.reserve(reserve_hint);
      free_list_.reserve(reserve_hint);
    }
  }

  // Fails if entry is null or already tracked by some queue/heap.
  Status insert(internal::CacheEntry* entry, Priority priority) {
    if (entry == nullptr || entry->metadata != nullptr) return Status::kFailure;
    Node* node = allocateNode(entry, priority);
    entry->metadata = node;
    heap_.push_back(node);
    node->heap_pos = heap_.size() - 1;
    siftUp(node->heap_pos);
    return Status::kSuccess;
  }

  // Repositions entry after its priority has changed. O(log n).
  Status updatePriority(internal::CacheEntry* entry, Priority priority) {
    if (!contains(entry)) return Status::kFailure;
    auto* node = static_cast<Node*>(entry->metadata);
    Priority old = node->priority;
    node->priority = priority;
    if (priority > old) {
      siftUp(node->heap_pos);
    } else if (priority < old) {
      siftDown(node->heap_pos);
    }
    return Status::kSuccess;
  }

  // Highest-priority entry without removing it; nullptr if empty.
  internal::CacheEntry* peek() const { return heap_.empty() ? nullptr : heap_.front()->cache_entry; }

  // Removes and returns the highest-priority entry; nullptr if empty.
  internal::CacheEntry* pop() {
    if (heap_.empty()) return nullptr;
    internal::CacheEntry* result = heap_.front()->cache_entry;
    removeAt(0);
    return result;
  }

  // Removes an arbitrary tracked entry, not necessarily the top (e.g. an
  // explicit invalidation/removal unrelated to normal eviction).
  Status remove(internal::CacheEntry* entry) {
    if (!contains(entry)) return Status::kFailure;
    removeAt(static_cast<Node*>(entry->metadata)->heap_pos);
    return Status::kSuccess;
  }

  bool contains(internal::CacheEntry* entry) const {
    if (entry == nullptr || entry->metadata == nullptr) return false;
    return static_cast<Node*>(entry->metadata)->owner == this;
  }

  // Precondition: contains(entry).
  Payload& payload(internal::CacheEntry* entry) {
    return static_cast<Node*>(entry->metadata)->payload;
  }
  const Payload& payload(internal::CacheEntry* entry) const {
    return static_cast<Node*>(entry->metadata)->payload;
  }

  bool empty() const { return heap_.empty(); }
  std::size_t size() const { return heap_.size(); }

 private:
  struct Node {
    internal::CacheEntry* cache_entry = nullptr;
    Priority priority{};
    std::size_t heap_pos = 0;
    PriorityQueue* owner = nullptr;
    [[no_unique_address]] Payload payload{};
  };

  Node* allocateNode(internal::CacheEntry* cache_entry, Priority priority) {
    Node* node;
    if (!free_list_.empty()) {
      node = free_list_.back();
      free_list_.pop_back();
    } else {
      pool_.emplace_back();
      node = &pool_.back();
    }
    node->cache_entry = cache_entry;
    node->priority = priority;
    node->owner = this;
    node->payload = Payload{};
    return node;
  }

  void freeNode(Node* node) { free_list_.push_back(node); }

  void swapNodes(std::size_t i, std::size_t j) {
    std::swap(heap_[i], heap_[j]);
    heap_[i]->heap_pos = i;
    heap_[j]->heap_pos = j;
  }

  void siftUp(std::size_t idx) {
    while (idx > 0) {
      std::size_t parent = (idx - 1) / 2;
      if (!(heap_[parent]->priority < heap_[idx]->priority)) break;
      swapNodes(parent, idx);
      idx = parent;
    }
  }

  void siftDown(std::size_t idx) {
    for (;;) {
      std::size_t left = 2 * idx + 1;
      std::size_t right = 2 * idx + 2;
      std::size_t largest = idx;
      if (left < heap_.size() && heap_[largest]->priority < heap_[left]->priority) largest = left;
      if (right < heap_.size() && heap_[largest]->priority < heap_[right]->priority) largest = right;
      if (largest == idx) break;
      swapNodes(idx, largest);
      idx = largest;
    }
  }

  void removeAt(std::size_t idx) {
    Node* node = heap_[idx];
    std::size_t last = heap_.size() - 1;
    if (idx != last) {
      swapNodes(idx, last);
    }
    heap_.pop_back();
    node->cache_entry->metadata = nullptr;
    freeNode(node);
    if (idx < heap_.size()) {
      // The element swapped into idx may need to move either way; only
      // one of these will actually do anything.
      siftDown(idx);
      siftUp(idx);
    }
  }

  std::deque<Node> pool_;  // pointer-stable: CacheEntry::metadata points into this
  std::vector<Node*> free_list_;
  std::vector<Node*> heap_;  // array-based heap of pointers into pool_
};

}  // namespace cachesim
