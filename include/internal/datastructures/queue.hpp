#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

#include "internal/cache/cache_entry.hpp"
#include "internal/common/compiler.hpp"
#include "internal/datastructures/block_pool.hpp"
#include "internal/datastructures/metadata.hpp"
#include "internal/status.hpp"

namespace cachesim {

// Placeholder for the Payload/Tag template arguments when a Queue does not
// need one. [[no_unique_address]] means an unused one costs nothing.
struct NoData {};

// Intrusive ordering list over cached objects, generalized with two optional
// pieces of caller-defined data:
//
//   - Payload: extra state carried PER OBJECT, alongside its node — Clock's
//     per-object reference bit is Queue<bool>, LRU-K's list of recent access
//     times is Queue<std::array<int64_t, K>>.
//   - Tag: one piece of state describing THIS QUEUE AS A WHOLE — LFU's "this
//     bucket is the objects with frequency N" is Queue<NoData, int64_t>.
//
// Each tracked object gets a QueueEntry node; CacheEntry::metadata points at
// it and the node points back at its CacheEntry, so both directions are O(1)
// with no hashtable in either. A policy may own as many independent Queues as
// it needs (LRU one, S3FIFO three, LFU one per frequency), and an entry is
// only ever in one of them at a time, so there is no conflict over the single
// metadata pointer.
//
// The node does not duplicate obj_id: cache_entry->objId() is one
// dereference away on a line the caller has almost always just touched
// anyway, and a second copy of an object's identity is a thing that can go
// out of sync with the first.
//
// Header-only: as a template, its methods must be visible wherever it is
// instantiated.
template <typename Payload = NoData, typename Tag = NoData>
class Queue {
 public:
  // reserve_hint: expected number of entries this queue will track, used to
  //   size the node pool in one allocation. 0 means "no hint".
  explicit Queue(std::size_t reserve_hint = 0, Tag tag = Tag{})
      : pool_(reserve_hint), tag_(std::move(tag)) {}

  Queue(const Queue&) = delete;
  Queue& operator=(const Queue&) = delete;
  // Not movable: nodes store the address of their owning queue, so moving a
  // Queue would leave every node pointing at the old address. Policies hold
  // their queues by value or in a container of pointers.
  Queue(Queue&&) = delete;
  Queue& operator=(Queue&&) = delete;

  [[nodiscard]] const Tag& tag() const { return tag_; }
  void setTag(Tag tag) { tag_ = std::move(tag); }

  // Fails if entry is null, or is already tracked by some queue or heap —
  // pushing it again would orphan the node it already has.
  Status pushFront(internal::CacheEntry* entry) {
    QueueEntry* node = acquire(entry);
    if (node == nullptr) return Status::kFailure;
    linkFront(node);
    return Status::kSuccess;
  }

  Status pushBack(internal::CacheEntry* entry) {
    QueueEntry* node = acquire(entry);
    if (node == nullptr) return Status::kFailure;
    linkBack(node);
    return Status::kSuccess;
  }

  // Fails if entry is null or not currently tracked by THIS queue.
  Status moveToFront(internal::CacheEntry* entry) {
    QueueEntry* node = nodeOf(entry);
    if (CACHESIM_UNLIKELY(node == nullptr)) return Status::kFailure;
    if (node != head_) {
      unlinkNode(node);
      linkFront(node);
    }
    return Status::kSuccess;
  }

  Status moveToBack(internal::CacheEntry* entry) {
    QueueEntry* node = nodeOf(entry);
    if (CACHESIM_UNLIKELY(node == nullptr)) return Status::kFailure;
    if (node != tail_) {
      unlinkNode(node);
      linkBack(node);
    }
    return Status::kSuccess;
  }

  // Detaches the entry from THIS queue's ordering and clears its
  // CacheEntry::metadata. The object is not removed from the cache — that is
  // CacheStructure::erase's job.
  Status unlink(internal::CacheEntry* entry) {
    QueueEntry* node = nodeOf(entry);
    if (CACHESIM_UNLIKELY(node == nullptr)) return Status::kFailure;
    unlinkNode(node);
    entry->metadata = nullptr;
    node->owner = nullptr;
    pool_.release(node);
    bytes_ -= entry->size();
    --count_;
    return Status::kSuccess;
  }

  // Removes and returns the entry at the back (the usual victim end for
  // LRU/FIFO), already detached. nullptr if empty. Saves the back() +
  // unlink() pair that every such policy would otherwise write, and with it
  // one redundant ownership check per eviction.
  internal::CacheEntry* popBack() {
    if (tail_ == nullptr) return nullptr;
    QueueEntry* node = tail_;
    internal::CacheEntry* entry = node->cache_entry;
    unlinkNode(node);
    entry->metadata = nullptr;
    node->owner = nullptr;
    pool_.release(node);
    bytes_ -= entry->size();
    --count_;
    return entry;
  }

  internal::CacheEntry* popFront() {
    if (head_ == nullptr) return nullptr;
    QueueEntry* node = head_;
    internal::CacheEntry* entry = node->cache_entry;
    unlinkNode(node);
    entry->metadata = nullptr;
    node->owner = nullptr;
    pool_.release(node);
    bytes_ -= entry->size();
    --count_;
    return entry;
  }

  // Whether entry is currently tracked by THIS queue specifically.
  [[nodiscard]] CACHESIM_ALWAYS_INLINE bool contains(
      const internal::CacheEntry* entry) const {
    return entry != nullptr && entry->metadata != nullptr && entry->metadata->owner == this;
  }

  // Precondition: contains(entry). Valid only while the entry stays in this
  // queue — a move to another queue allocates a fresh node, and the payload
  // does not follow it.
  [[nodiscard]] Payload& payload(internal::CacheEntry* entry) {
    return static_cast<QueueEntry*>(entry->metadata)->payload;
  }
  [[nodiscard]] const Payload& payload(const internal::CacheEntry* entry) const {
    return static_cast<const QueueEntry*>(entry->metadata)->payload;
  }

  // Reads the payload off an entry's node without checking which Queue
  // instance owns it.
  //
  // Precondition: the entry's metadata node really is this Queue
  // specialization's node type — i.e. it is tracked by *some*
  // Queue<Payload, Tag> with these exact template arguments. That is weaker
  // than contains(), and it is what a policy holding many same-typed queues
  // needs: LFU keeps one bucket per frequency and has to read an object's
  // frequency *in order to find out which bucket it is in*, so it cannot ask
  // the owning bucket first. The payload sits at a fixed offset in the node,
  // so this is a field load, not a search over buckets.
  [[nodiscard]] static Payload& payloadOf(internal::CacheEntry* entry) {
    return static_cast<QueueEntry*>(entry->metadata)->payload;
  }
  [[nodiscard]] static const Payload& payloadOf(const internal::CacheEntry* entry) {
    return static_cast<const QueueEntry*>(entry->metadata)->payload;
  }

  // nullptr if the queue is empty.
  [[nodiscard]] internal::CacheEntry* front() const {
    return head_ != nullptr ? head_->cache_entry : nullptr;
  }
  [[nodiscard]] internal::CacheEntry* back() const {
    return tail_ != nullptr ? tail_->cache_entry : nullptr;
  }

  // One step towards the back / towards the front from an entry in this
  // queue. nullptr at the end. Lets a policy walk the ordering (Clock's hand,
  // Sieve's hand, SLRU's demotion scan) without exposing the node type.
  [[nodiscard]] internal::CacheEntry* next(const internal::CacheEntry* entry) const {
    const QueueEntry* node = static_cast<const QueueEntry*>(entry->metadata);
    return node->next != nullptr ? node->next->cache_entry : nullptr;
  }
  [[nodiscard]] internal::CacheEntry* prev(const internal::CacheEntry* entry) const {
    const QueueEntry* node = static_cast<const QueueEntry*>(entry->metadata);
    return node->prev != nullptr ? node->prev->cache_entry : nullptr;
  }

  [[nodiscard]] bool empty() const { return head_ == nullptr; }
  [[nodiscard]] std::size_t size() const { return count_; }

  // Total size of the objects in this queue. Policies that split a byte
  // capacity between queues (ARC's target, S3FIFO's 10/90 split) need this,
  // and keeping it here is O(1) per link instead of O(n) per query.
  [[nodiscard]] std::uint64_t bytes() const { return bytes_; }

  // Corrects the byte total after a tracked object's size changed underneath
  // the queue. Only the policy knows when that happened (its onResize hook),
  // and only this queue knows the running total, so the two have to meet.
  void adjustBytes(std::int64_t delta) {
    bytes_ = static_cast<std::uint64_t>(static_cast<std::int64_t>(bytes_) + delta);
  }

  // Detaches everything. Clears metadata on every tracked entry, so the
  // entries are safe for the cache structure to recycle afterwards.
  void clear() {
    for (QueueEntry* node = head_; node != nullptr;) {
      QueueEntry* next_node = node->next;
      node->cache_entry->metadata = nullptr;
      node->owner = nullptr;
      node = next_node;
    }
    head_ = nullptr;
    tail_ = nullptr;
    count_ = 0;
    bytes_ = 0;
    pool_.releaseAll();
  }

  [[nodiscard]] std::uint64_t metadataBytes() const { return pool_.memoryBytes(); }

 private:
  struct QueueEntry : internal::MetadataNode {
    internal::CacheEntry* cache_entry = nullptr;
    QueueEntry* prev = nullptr;
    QueueEntry* next = nullptr;
    [[no_unique_address]] Payload payload{};
  };

  // Takes a fresh node for `entry` and points the entry at it, or returns
  // nullptr if the entry is already tracked somewhere.
  QueueEntry* acquire(internal::CacheEntry* entry) {
    if (CACHESIM_UNLIKELY(entry == nullptr || entry->metadata != nullptr)) return nullptr;
    QueueEntry* node = pool_.allocate();
    node->owner = this;
    node->cache_entry = entry;
    node->prev = nullptr;
    node->next = nullptr;
    node->payload = Payload{};
    entry->metadata = node;
    bytes_ += entry->size();
    ++count_;
    return node;
  }

  QueueEntry* nodeOf(internal::CacheEntry* entry) const {
    if (entry == nullptr || entry->metadata == nullptr || entry->metadata->owner != this) {
      return nullptr;
    }
    return static_cast<QueueEntry*>(entry->metadata);
  }

  void linkFront(QueueEntry* node) {
    node->prev = nullptr;
    node->next = head_;
    if (head_ != nullptr) head_->prev = node;
    head_ = node;
    if (tail_ == nullptr) tail_ = node;
  }

  void linkBack(QueueEntry* node) {
    node->next = nullptr;
    node->prev = tail_;
    if (tail_ != nullptr) tail_->next = node;
    tail_ = node;
    if (head_ == nullptr) head_ = node;
  }

  void unlinkNode(QueueEntry* node) {
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

  internal::BlockPool<QueueEntry> pool_;
  QueueEntry* head_ = nullptr;
  QueueEntry* tail_ = nullptr;
  std::size_t count_ = 0;
  std::uint64_t bytes_ = 0;
  [[no_unique_address]] Tag tag_{};
};

}  // namespace cachesim
