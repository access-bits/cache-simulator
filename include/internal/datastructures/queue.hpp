#pragma once

#include <cstddef>
#include <deque>
#include <utility>
#include <vector>

#include "internal/cache/cache_entry.hpp"
#include "internal/status.hpp"

namespace cachesim {

// Placeholder for the Payload/Tag template arguments when a Queue doesn't
// need one. [[no_unique_address]] means it costs nothing when unused.
struct NoData {};

// Intrusive ordering list over cached objects, generalized with two
// optional pieces of caller-defined data:
//   - Payload: extra state carried PER OBJECT, alongside its node (e.g.
//     Clock's per-object reference bit — Queue<bool>).
//   - Tag: one piece of state describing THIS QUEUE AS A WHOLE (e.g. LFU's
//     "this bucket is for frequency N" — Queue<NoData, int64_t>).
//
// Each tracked object gets a QueueEntry node; CacheEntry::metadata points
// at it, and the node points back at its CacheEntry — CacheEntry ->
// QueueEntry and QueueEntry -> CacheEntry, both directions available, no
// hashtable lookups needed in either direction.
//
// A policy may create as many independent Queues as it needs (LRU needs
// one; ARC would need four for T1/T2/B1/B2; LFU needs one per frequency,
// each tagged with that frequency) — a CacheEntry only ever has one live
// QueueEntry at a time, so there's no conflict over the metadata pointer.
// Queue has no dependency on CacheStructure: callers always already hold
// the CacheEntry* they want to operate on.
//
// Header-only: as a template, its methods must be visible wherever it's
// instantiated, so there is no matching queue.cpp.
template <typename Payload = NoData, typename Tag = NoData>
class Queue {
 public:
  // reserve_hint: expected number of entries this queue will track, if
  // known, to pre-size the free-list reuse pool. 0 means "no hint".
  explicit Queue(std::size_t reserve_hint = 0, Tag tag = Tag{}) : tag_(std::move(tag)) {
    if (reserve_hint > 0) {
      free_list_.reserve(reserve_hint);
    }
  }

  const Tag& tag() const { return tag_; }
  void setTag(Tag tag) { tag_ = std::move(tag); }

  // Fails if entry is null, or entry is already tracked by some queue
  // (pushing it again would orphan its existing node).
  Status pushFront(internal::CacheEntry* entry) {
    if (entry == nullptr || entry->metadata != nullptr) return Status::kFailure;
    QueueEntry* node = allocateNode(entry);
    entry->metadata = node;
    linkFront(node);
    return Status::kSuccess;
  }

  Status pushBack(internal::CacheEntry* entry) {
    if (entry == nullptr || entry->metadata != nullptr) return Status::kFailure;
    QueueEntry* node = allocateNode(entry);
    entry->metadata = node;
    linkBack(node);
    return Status::kSuccess;
  }

  // Fail if entry is null or not currently tracked by this queue.
  Status moveToFront(internal::CacheEntry* entry) {
    if (!contains(entry)) return Status::kFailure;
    auto* node = static_cast<QueueEntry*>(entry->metadata);
    if (node != head_) {
      unlinkNode(node);
      linkFront(node);
    }
    return Status::kSuccess;
  }

  Status moveToBack(internal::CacheEntry* entry) {
    if (!contains(entry)) return Status::kFailure;
    auto* node = static_cast<QueueEntry*>(entry->metadata);
    if (node != tail_) {
      unlinkNode(node);
      linkBack(node);
    }
    return Status::kSuccess;
  }

  // Detaches the entry from THIS queue's ordering and clears its
  // CacheEntry::metadata. The object is not removed from the cache —
  // that's CacheStructure::erase's job. Fails under the same conditions
  // as moveToFront/moveToBack.
  Status unlink(internal::CacheEntry* entry) {
    if (!contains(entry)) return Status::kFailure;
    auto* node = static_cast<QueueEntry*>(entry->metadata);
    unlinkNode(node);
    entry->metadata = nullptr;
    freeNode(node);
    return Status::kSuccess;
  }

  // Whether entry is currently tracked by THIS queue specifically (not
  // just some queue — an entry's metadata is only ever owned by one).
  bool contains(internal::CacheEntry* entry) const {
    if (entry == nullptr || entry->metadata == nullptr) return false;
    return static_cast<QueueEntry*>(entry->metadata)->owner == this;
  }

  // Precondition: contains(entry). Valid only while entry stays in this queue.
  Payload& payload(internal::CacheEntry* entry) {
    return static_cast<QueueEntry*>(entry->metadata)->payload;
  }
  const Payload& payload(internal::CacheEntry* entry) const {
    return static_cast<QueueEntry*>(entry->metadata)->payload;
  }

  // nullptr if the queue is empty.
  internal::CacheEntry* front() const { return head_ != nullptr ? head_->cache_entry : nullptr; }
  internal::CacheEntry* back() const { return tail_ != nullptr ? tail_->cache_entry : nullptr; }
  bool empty() const { return head_ == nullptr; }

 private:
  struct QueueEntry {
    internal::CacheEntry* cache_entry = nullptr;
    QueueEntry* prev_ = nullptr;
    QueueEntry* next_ = nullptr;
    Queue* owner = nullptr;  // disambiguates which queue owns this node
    [[no_unique_address]] Payload payload{};
  };

  QueueEntry* allocateNode(internal::CacheEntry* cache_entry) {
    QueueEntry* node;
    if (!free_list_.empty()) {
      node = free_list_.back();
      free_list_.pop_back();
    } else {
      pool_.emplace_back();
      node = &pool_.back();
    }
    node->cache_entry = cache_entry;
    node->prev_ = nullptr;
    node->next_ = nullptr;
    node->owner = this;
    node->payload = Payload{};
    return node;
  }

  void freeNode(QueueEntry* node) { free_list_.push_back(node); }

  void linkFront(QueueEntry* node) {
    node->prev_ = nullptr;
    node->next_ = head_;
    if (head_ != nullptr) {
      head_->prev_ = node;
    }
    head_ = node;
    if (tail_ == nullptr) {
      tail_ = node;
    }
  }

  void linkBack(QueueEntry* node) {
    node->next_ = nullptr;
    node->prev_ = tail_;
    if (tail_ != nullptr) {
      tail_->next_ = node;
    }
    tail_ = node;
    if (head_ == nullptr) {
      head_ = node;
    }
  }

  void unlinkNode(QueueEntry* node) {
    if (node->prev_ != nullptr) {
      node->prev_->next_ = node->next_;
    } else {
      head_ = node->next_;
    }
    if (node->next_ != nullptr) {
      node->next_->prev_ = node->prev_;
    } else {
      tail_ = node->prev_;
    }
    node->prev_ = nullptr;
    node->next_ = nullptr;
  }

  std::deque<QueueEntry> pool_;  // pointer-stable: CacheEntry::metadata points into this
  std::vector<QueueEntry*> free_list_;
  QueueEntry* head_ = nullptr;
  QueueEntry* tail_ = nullptr;
  Tag tag_{};
};

}  // namespace cachesim
