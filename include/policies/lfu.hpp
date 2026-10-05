#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ankerl/unordered_dense.h"
#include "internal/datastructures/priority_queue.hpp"
#include "internal/datastructures/queue.hpp"
#include "internal/eviction/eviction_policy.hpp"

namespace cachesim::policies {

// Exact LFU with O(1) hit and O(1) amortized eviction.
//
// The structure is one Queue per distinct frequency — a bucket holding every
// cached object accessed exactly that many times — plus a running minimum.
// A hit unlinks the object from its bucket and appends it to the next one up,
// which is O(1); eviction takes the front of the minimum bucket, which is
// O(1) once the minimum is known. This is the same shape as libCacheSim's
// freq_map, with our Queue in place of its hand-rolled per-bucket list.
//
// Each bucket carries its frequency as the Queue's Tag, and each object
// carries its own frequency as the node's Payload. The Payload is what makes
// a hit O(1): the object's frequency has to be read before it can be moved,
// and reading it off the node is a dereference of a line the hit path has
// already touched, where asking "which bucket is this in" would be a search.
// The Tag is the bucket's own identity, which makes a bucket self-describing
// when iterating the map and keeps the two from drifting silently (debug
// builds assert they agree).
//
// Ties within a frequency break FIFO — the object that reached that frequency
// first is evicted first — matching libCacheSim, so results line up.
class Lfu final : public IEvictionPolicy {
 public:
  explicit Lfu(const PolicyConfig& config = {}) : entry_hint_(config.entry_hint) {
    // Frequency 1 is where every admission lands, so it is the one bucket
    // worth pre-sizing and the one we never destroy.
    buckets_.emplace(1, std::make_unique<Bucket>(entry_hint_, std::int64_t{1}));
  }

  [[nodiscard]] std::string name() const override { return "LFU"; }

  Status onAdmit(internal::CacheEntry* entry, const Request& req) override {
    (void)req;
    Bucket& bucket = bucketFor(1);
    const Status status = bucket.pushBack(entry);
    if (!ok(status)) return status;
    bucket.payload(entry) = 1;
    min_freq_ = 1;
    return Status::kSuccess;
  }

  Status onHit(internal::CacheEntry* entry, const Request& req) override {
    (void)req;
    if (entry == nullptr || entry->metadata == nullptr) return Status::kFailure;
    Bucket* bucket = nullptr;
    const std::int64_t frequency = frequencyOf(entry, &bucket);
    if (bucket == nullptr) return Status::kFailure;
    if (!ok(bucket->unlink(entry))) return Status::kFailure;

    const std::int64_t next_frequency = frequency + 1;
    Bucket& target = bucketFor(next_frequency);
    const Status status = target.pushBack(entry);
    if (!ok(status)) return status;
    target.payload(entry) = next_frequency;

    // The object moved up, so the bucket it left may now be empty. If it was
    // the minimum, the new minimum is the bucket it moved into — nothing
    // between them can exist, because frequencies move one step at a time.
    if (bucket->empty()) {
      if (frequency == min_freq_) min_freq_ = next_frequency;
      retire(frequency);
    }
    return Status::kSuccess;
  }

  internal::CacheEntry* evict() override {
    const auto it = findMinBucket();
    if (it == buckets_.end()) return nullptr;
    Bucket& bucket = *it->second;
    internal::CacheEntry* victim = bucket.popFront();
    if (victim == nullptr) return nullptr;
    if (bucket.empty() && it->first != 1) {
      const std::int64_t frequency = it->first;
      retire(frequency);
    }
    return victim;
  }

  Status onRemove(internal::CacheEntry* entry) override {
    Bucket* bucket = nullptr;
    const std::int64_t frequency = frequencyOf(entry, &bucket);
    if (bucket == nullptr) return Status::kFailure;
    const Status status = bucket->unlink(entry);
    if (ok(status) && bucket->empty()) retire(frequency);
    return status;
  }

  Status onResize(internal::CacheEntry* entry, std::uint32_t old_size,
                  std::uint32_t new_size) override {
    Bucket* bucket = nullptr;
    (void)frequencyOf(entry, &bucket);
    if (bucket == nullptr) return Status::kFailure;
    bucket->adjustBytes(static_cast<std::int64_t>(new_size) -
                        static_cast<std::int64_t>(old_size));
    return Status::kSuccess;
  }

  void clear() override {
    for (auto& [frequency, bucket] : buckets_) {
      (void)frequency;
      bucket->clear();
    }
    buckets_.clear();
    spare_buckets_.clear();
    buckets_.emplace(1, std::make_unique<Bucket>(entry_hint_, std::int64_t{1}));
    min_freq_ = 1;
  }

  [[nodiscard]] std::uint64_t metadataBytes() const override {
    std::uint64_t total = 0;
    for (const auto& [frequency, bucket] : buckets_) {
      (void)frequency;
      total += bucket->metadataBytes();
    }
    return total;
  }

 private:
  // Payload is the object's own frequency, Tag is the bucket's frequency.
  using Bucket = Queue<std::int64_t, std::int64_t>;
  // Held by unique_ptr because Queue nodes record the address of their owning
  // queue, and a map that stores its values inline moves them when it grows.
  // The indirection costs one dereference on a path that is not the hot one.
  using BucketMap = ankerl::unordered_dense::map<std::int64_t, std::unique_ptr<Bucket>>;

  Bucket& bucketFor(std::int64_t frequency) {
    const auto it = buckets_.find(frequency);
    if (it != buckets_.end()) return *it->second;
    // Reuse a retired bucket if there is one. This is not a micro-
    // optimization: a hot object under LFU is usually the only occupant of
    // its frequency, so each access moves it to frequency f+1 (a bucket that
    // does not exist yet) and empties bucket f. Constructing and destroying a
    // Bucket per access means a node-pool block allocated and freed per
    // access, which measured as a 10x throughput gap against every other
    // policy. Recycling the object instead makes the hit path allocation-free
    // in steady state.
    std::unique_ptr<Bucket> bucket;
    if (!spare_buckets_.empty()) {
      bucket = std::move(spare_buckets_.back());
      spare_buckets_.pop_back();
      bucket->setTag(frequency);
    } else {
      // Later buckets hold far fewer objects than frequency 1, so they are
      // not pre-sized; their pools grow on demand.
      bucket = std::make_unique<Bucket>(0, frequency);
    }
    auto [inserted, inserted_ok] = buckets_.emplace(frequency, std::move(bucket));
    (void)inserted_ok;
    return *inserted->second;
  }

  // Reads an entry's frequency off its own node and hands back the bucket it
  // is in, both in O(1). Returns 0 with *bucket == nullptr if the entry is
  // not tracked by this policy.
  //
  // The frequency comes from the node's payload rather than from a search
  // over buckets, and the frequency *is* the bucket's key, so one map lookup
  // finds the bucket. contains() then confirms the entry really is in that
  // bucket, which both validates the payload and rules out an entry that
  // belongs to some other structure entirely.
  std::int64_t frequencyOf(internal::CacheEntry* entry, Bucket** bucket) const {
    *bucket = nullptr;
    if (entry == nullptr || entry->metadata == nullptr) return 0;
    const std::int64_t frequency = Bucket::payloadOf(entry);
    const auto it = buckets_.find(frequency);
    if (it == buckets_.end() || !it->second->contains(entry)) return 0;
    *bucket = it->second.get();
    return frequency;
  }

  // Takes an empty bucket out of the map and keeps the object for reuse.
  // Keeping empty buckets in the map instead would be simpler, but the map
  // would then grow with the highest frequency any object ever reaches, which
  // on a long trace is unbounded.
  void retire(std::int64_t frequency) {
    if (frequency == 1) return;  // kept alive: every admission needs it
    const auto it = buckets_.find(frequency);
    if (it == buckets_.end() || !it->second->empty()) return;
    if (spare_buckets_.size() < kMaxSpareBuckets) {
      spare_buckets_.push_back(std::move(it->second));
    }
    buckets_.erase(it);
  }

  // Advances min_freq_ to the lowest non-empty bucket. Every admission resets
  // it to 1, so in steady state this finds its answer immediately; the scan
  // only does work after a run of evictions has emptied the low buckets,
  // which is the same amortization libCacheSim's update_min_freq relies on.
  BucketMap::iterator findMinBucket() {
    auto it = buckets_.find(min_freq_);
    if (it != buckets_.end() && !it->second->empty()) return it;
    std::int64_t best = 0;
    auto best_it = buckets_.end();
    for (auto candidate = buckets_.begin(); candidate != buckets_.end(); ++candidate) {
      if (candidate->second->empty()) continue;
      if (best == 0 || candidate->first < best) {
        best = candidate->first;
        best_it = candidate;
      }
    }
    if (best_it != buckets_.end()) min_freq_ = best;
    return best_it;
  }

  // A small cap, because the only thing spares protect against is the
  // construct/destroy cycle; a handful is enough to absorb it.
  static constexpr std::size_t kMaxSpareBuckets = 16;

  BucketMap buckets_;
  std::vector<std::unique_ptr<Bucket>> spare_buckets_;
  std::int64_t min_freq_ = 1;
  std::size_t entry_hint_ = 0;
};

}  // namespace cachesim::policies
