#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "ankerl/unordered_dense.h"
#include "internal/datastructures/block_pool.hpp"

namespace cachesim {

// Bounded FIFO record of object ids, with O(1) membership, O(1) insertion,
// O(1) removal of a specific id, and O(1) removal of the oldest.
//
// This is the "ghost list" structure. ARC, 2Q and S3FIFO all remember
// something about objects that have been evicted from the real cache, and an
// evicted object has no CacheEntry left to hang metadata off — its slot has
// been recycled for something else. So unlike Queue and PriorityQueue, this
// one works with bare obj_ids.
//
// Each id carries the size the object had when it was recorded, and the class
// keeps a running byte total. ARC needs it: its adaptation compares the sizes
// of its two ghost lists against the cache's capacity, and on a byte-capacity
// cache "size" has to mean bytes, which cannot be recovered from an id once
// the object itself is gone.
//
// Implementation note, because the obvious cheaper design is wrong. A ring
// buffer of ids plus an id -> slot map looks like it fits: record writes at
// the head, the oldest falls off the tail, and removing a specific id leaves
// a tombstone so that it stays O(1). It does not work, because the capacity
// then bounds *slots* rather than *live ids*. Removal by id is the dominant
// operation here — almost every ghost leaves by being promoted back into the
// cache — so the ring fills with tombstones scattered through it, reaches its
// bound while only a fraction of its ids are live, and starts dropping live
// ghosts to make room. ARC then adapts p on a directory that has quietly
// lost entries, which costs a point or two of hit ratio with nothing to
// indicate anything is wrong.
//
// So: an intrusive doubly-linked list over a pooled node per id, plus an
// id -> node map. Every operation is O(1), the capacity bounds exactly the
// number of live ids, and there is nothing to compact.
class IdHistory {
 public:
  struct Record {
    std::uint64_t obj_id = 0;
    std::uint32_t size = 0;
  };

  explicit IdHistory(std::size_t capacity) : pool_(capacity), capacity_(capacity) {
    if (capacity > 0) index_.reserve(capacity);
  }

  IdHistory(const IdHistory&) = delete;
  IdHistory& operator=(const IdHistory&) = delete;

  [[nodiscard]] bool contains(std::uint64_t obj_id) const { return index_.contains(obj_id); }

  // The size recorded with obj_id, or 0 if it is not recorded.
  [[nodiscard]] std::uint32_t sizeOf(std::uint64_t obj_id) const {
    const auto it = index_.find(obj_id);
    return it != index_.end() ? it->second->size : 0;
  }

  // Records obj_id as the newest entry, dropping the oldest if that would
  // exceed capacity. No-op if obj_id is already recorded, and no-op entirely
  // for a zero-capacity history.
  void record(std::uint64_t obj_id, std::uint32_t size = 0) {
    if (capacity_ == 0 || index_.contains(obj_id)) return;
    if (index_.size() >= capacity_) popOldest();
    Node* node = pool_.allocate();
    node->obj_id = obj_id;
    node->size = size;
    node->prev = nullptr;
    node->next = head_;
    if (head_ != nullptr) head_->prev = node;
    head_ = node;
    if (tail_ == nullptr) tail_ = node;
    index_[obj_id] = node;
    bytes_ += size;
  }

  // Removes obj_id if recorded. Returns whether it was.
  bool erase(std::uint64_t obj_id) {
    const auto it = index_.find(obj_id);
    if (it == index_.end()) return false;
    Node* node = it->second;
    bytes_ -= node->size;
    unlink(node);
    index_.erase(it);
    pool_.release(node);
    return true;
  }

  // Removes and returns the oldest recorded id, or nullopt if empty. ARC
  // drives its ghost lists this way: its replace step pops the least recently
  // seen ghost to make room, rather than waiting for a capacity bound.
  std::optional<Record> popOldest() {
    if (tail_ == nullptr) return std::nullopt;
    Node* node = tail_;
    const Record record{node->obj_id, node->size};
    bytes_ -= node->size;
    unlink(node);
    index_.erase(record.obj_id);
    pool_.release(node);
    return record;
  }

  [[nodiscard]] std::size_t size() const { return index_.size(); }
  [[nodiscard]] std::uint64_t bytes() const { return bytes_; }
  [[nodiscard]] std::size_t capacity() const { return capacity_; }
  [[nodiscard]] bool empty() const { return index_.empty(); }

  // The newest and oldest recorded ids, without removing them.
  [[nodiscard]] std::optional<Record> newest() const {
    if (head_ == nullptr) return std::nullopt;
    return Record{head_->obj_id, head_->size};
  }
  [[nodiscard]] std::optional<Record> oldest() const {
    if (tail_ == nullptr) return std::nullopt;
    return Record{tail_->obj_id, tail_->size};
  }

  void clear() {
    index_.clear();
    head_ = nullptr;
    tail_ = nullptr;
    bytes_ = 0;
    pool_.releaseAll();
  }

  [[nodiscard]] std::uint64_t metadataBytes() const {
    return pool_.memoryBytes() +
           static_cast<std::uint64_t>(index_.size()) * (sizeof(std::uint64_t) + sizeof(void*));
  }

 private:
  struct Node {
    Node* prev = nullptr;
    Node* next = nullptr;
    std::uint64_t obj_id = 0;
    std::uint32_t size = 0;
  };

  void unlink(Node* node) {
    if (node->prev != nullptr) {
      node->prev->next = node->next;
    } else {
      head_ = node->next;
    }
    if (node->next != nullptr) {
      node->next->prev = node->prev;
    } else {
      tail_ = node->prev;
    }
    node->prev = nullptr;
    node->next = nullptr;
  }

  internal::BlockPool<Node> pool_;
  ankerl::unordered_dense::map<std::uint64_t, Node*> index_;
  Node* head_ = nullptr;  // newest
  Node* tail_ = nullptr;  // oldest
  std::size_t capacity_ = 0;
  std::uint64_t bytes_ = 0;
};

}  // namespace cachesim
