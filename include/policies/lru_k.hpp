#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>

#include "internal/datastructures/priority_queue.hpp"
#include "internal/eviction/eviction_policy.hpp"

namespace cachesim::policies {

// LRU-K (O'Neil, O'Neil & Weikum, SIGMOD'93): evict by the time of the K-th
// most recent reference instead of the 1st.
//
// LRU's weakness is that it cannot tell a hot object from an object touched
// once, a moment ago — both look equally recent. LRU-K looks K references
// back, so one touch is not enough to look hot: with K=2, an object has to be
// referenced twice within the cache's memory to outrank an object referenced
// twice recently. That single change makes it far more robust to scans and to
// bursty one-off traffic, and LRU-2 is the usual choice (K=1 is exactly LRU).
//
// The victim is the object whose K-th most recent reference is oldest.
// Objects with fewer than K references so far have no K-th reference at all;
// they are infinitely old by definition and go first, ordered among
// themselves by first reference (FIFO), which is the conventional reading.
//
// Implemented as an addressable min-heap keyed on
// (kth_reference_time, last_reference_time) and a per-object ring of the last
// K reference times in the heap node's payload. Time is the logical request
// counter, not trace wall-clock: it is dense, monotone and never ties.
//
// Parameter: k=K (default 2, capped at 8 — beyond that the extra history
// buys nothing measurable and the payload grows).
class LruK final : public IEvictionPolicy {
 public:
  static constexpr std::size_t kMaxK = 8;

  explicit LruK(const PolicyConfig& config = {})
      : k_(static_cast<std::size_t>(std::clamp<std::int64_t>(config.params.getInt("k", 2), 1,
                                                             static_cast<std::int64_t>(kMaxK)))),
        heap_(config.entry_hint) {}

  [[nodiscard]] std::string name() const override { return "LRU-" + std::to_string(k_); }

  Status onAdmit(internal::CacheEntry* entry, const Request& req) override {
    (void)req;
    const std::int64_t now = ++clock_;
    const Status status = heap_.insert(entry, Key{kUnseen, now});
    if (!ok(status)) return status;
    History& history = heap_.payload(entry);
    history = History{};
    history.record(now);
    // The key has to be recomputed from the history rather than assumed to
    // be kUnseen: for K=1 an admission already *is* the first reference, so
    // the object's 1st-most-recent reference time is now. Leaving it at
    // kUnseen would make LRU-1 evict by insertion order instead of by
    // recency, i.e. not LRU at all.
    return heap_.updatePriority(entry, Key{history.kth(k_), now});
  }

  Status onHit(internal::CacheEntry* entry, const Request& req) override {
    (void)req;
    if (!heap_.contains(entry)) return Status::kFailure;
    const std::int64_t now = ++clock_;
    History& history = heap_.payload(entry);
    history.record(now);
    return heap_.updatePriority(entry, Key{history.kth(k_), now});
  }

  internal::CacheEntry* evict() override { return heap_.pop(); }

  Status onRemove(internal::CacheEntry* entry) override { return heap_.remove(entry); }

  Status onResize(internal::CacheEntry* entry, std::uint32_t old_size,
                  std::uint32_t new_size) override {
    (void)entry;
    heap_.adjustBytes(static_cast<std::int64_t>(new_size) - static_cast<std::int64_t>(old_size));
    return Status::kSuccess;
  }

  void clear() override {
    heap_.clear();
    clock_ = 0;
  }

  [[nodiscard]] std::uint64_t metadataBytes() const override { return heap_.metadataBytes(); }

 private:
  // Sorts below every real reference time, so an object without K references
  // yet is always a better victim than one that has them.
  static constexpr std::int64_t kUnseen = -1;

  // (kth reference time, most recent reference time). The second component is
  // only a tie-break, and it only ever comes into play between objects that
  // both lack a K-th reference — there it orders them by recency of their
  // single reference, i.e. LRU.
  using Key = std::pair<std::int64_t, std::int64_t>;

  // The last K reference times, newest first, as a fixed ring. Fixed rather
  // than a side container because it rides inside the heap node: one cache
  // line, no indirection, and nothing to allocate per object.
  struct History {
    std::array<std::int64_t, kMaxK> times{};
    std::uint8_t count = 0;
    std::uint8_t head = 0;  // index of the newest entry

    void record(std::int64_t now) {
      head = static_cast<std::uint8_t>((head + kMaxK - 1) % kMaxK);
      times[head] = now;
      if (count < kMaxK) ++count;
    }

    // Time of the k-th most recent reference, or kUnseen if there have been
    // fewer than k references.
    [[nodiscard]] std::int64_t kth(std::size_t k) const {
      if (count < k) return kUnseen;
      return times[(head + k - 1) % kMaxK];
    }
  };

  std::size_t k_;
  // Min-heap: std::greater puts the smallest key at the root, and the
  // smallest key is the oldest K-th reference.
  PriorityQueue<Key, History, std::greater<Key>> heap_;
  std::int64_t clock_ = 0;
};

}  // namespace cachesim::policies
