#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

#include "internal/datastructures/id_history.hpp"
#include "internal/datastructures/queue.hpp"
#include "internal/eviction/eviction_policy.hpp"

namespace cachesim::policies {

// S3-FIFO (Yang et al., SOSP'23): three FIFO queues, no linked-list surgery
// on the hot path, and hit ratios at or above ARC's on most published traces.
//
// The observation it is built on is that the overwhelming majority of objects
// in a real cache workload are requested once and never again. Any policy
// that admits those objects into its main structure pays for them twice —
// once in space, once in the eviction work to get rid of them. So:
//
//   S  a small FIFO, 10% of capacity, where every object missing for the
//      first time lands. One-hit-wonders live and die here without ever
//      touching the main queue.
//   M  the main FIFO, 90% of capacity, holding objects that proved reuse
//      while in S (or that were remembered in G).
//   G  a ghost FIFO of ids evicted from S without proving anything. A later
//      miss on a remembered id is proof of reuse at a longer distance, so
//      that object goes straight into M.
//
// A hit anywhere just bumps a 2-bit counter on the object — no pointer
// writes, which is what makes the hot path cheap. The counter is spent at
// eviction time: in S it decides promote-to-M versus evict-and-ghost; in M it
// buys a lap of the queue, which makes M behave like CLOCK.
//
// Parameters:
//   small-size-ratio=f   (default 0.10) S's share of capacity
//   ghost-size-ratio=f   (default 0.90) G's size, as a multiple of the
//                        cache's expected object count
//   move-to-main-threshold=k (default 1) hits in S needed to earn promotion
class S3Fifo final : public IEvictionPolicy {
 public:
  explicit S3Fifo(const PolicyConfig& config = {})
      : capacity_bytes_(config.capacity_bytes),
        small_ratio_(std::clamp(config.params.getDouble("small-size-ratio", 0.10), 0.01, 0.99)),
        promote_threshold_(static_cast<std::uint8_t>(std::clamp<std::int64_t>(
            config.params.getInt("move-to-main-threshold", 1), 1, 3))),
        small_(std::max<std::size_t>(16, static_cast<std::size_t>(
                                             static_cast<double>(config.entry_hint) *
                                             small_ratio_))),
        main_(config.entry_hint),
        ghost_(ghostCapacity(config)) {
    small_bytes_budget_ =
        static_cast<std::uint64_t>(static_cast<double>(capacity_bytes_) * small_ratio_);
  }

  [[nodiscard]] std::string name() const override { return "S3FIFO"; }

  Status onMiss(const Request& req) override {
    // Remembered in G: this id has already been through S once, so it has
    // earned a place in M directly.
    admit_to_main_ = ghost_.erase(req.obj_id);
    return Status::kSuccess;
  }

  Status onAdmit(internal::CacheEntry* entry, const Request& req) override {
    (void)req;
    Queue<std::uint8_t>& target = admit_to_main_ ? main_ : small_;
    admit_to_main_ = false;
    const Status status = target.pushFront(entry);
    if (!ok(status)) return status;
    target.payload(entry) = 0;
    return Status::kSuccess;
  }

  Status onHit(internal::CacheEntry* entry, const Request& req) override {
    (void)req;
    // Saturating 2-bit counter, and that is the entire hot path. The object
    // does not move, which is the point.
    if (small_.contains(entry)) {
      std::uint8_t& freq = small_.payload(entry);
      if (freq < 3) ++freq;
      return Status::kSuccess;
    }
    if (main_.contains(entry)) {
      std::uint8_t& freq = main_.payload(entry);
      if (freq < 3) ++freq;
      return Status::kSuccess;
    }
    return Status::kFailure;
  }

  internal::CacheEntry* evict() override {
    // Both branches can do work without producing a victim — a promotion from
    // S to M, or a lap of M — so this loops until an object actually leaves
    // the cache. Each iteration either returns, or spends one counter
    // increment, or moves one object out of S, so it terminates.
    for (;;) {
      if (small_.empty() && main_.empty()) return nullptr;
      // The paper's condition is |S| >= 0.1C, not >. The difference shows up
      // on every eviction where S is sitting exactly at its share, which on a
      // steady workload is most of them.
      if (!small_.empty() && (small_.bytes() >= small_bytes_budget_ || main_.empty())) {
        if (internal::CacheEntry* victim = evictFromSmall(); victim != nullptr) return victim;
        continue;
      }
      if (internal::CacheEntry* victim = evictFromMain(); victim != nullptr) return victim;
    }
  }

  Status onRemove(internal::CacheEntry* entry) override {
    if (small_.contains(entry)) return small_.unlink(entry);
    if (main_.contains(entry)) return main_.unlink(entry);
    return Status::kFailure;
  }

  Status onResize(internal::CacheEntry* entry, std::uint32_t old_size,
                  std::uint32_t new_size) override {
    const auto delta =
        static_cast<std::int64_t>(new_size) - static_cast<std::int64_t>(old_size);
    if (small_.contains(entry)) {
      small_.adjustBytes(delta);
      return Status::kSuccess;
    }
    if (main_.contains(entry)) {
      main_.adjustBytes(delta);
      return Status::kSuccess;
    }
    return Status::kFailure;
  }

  void clear() override {
    small_.clear();
    main_.clear();
    ghost_.clear();
    admit_to_main_ = false;
  }

  [[nodiscard]] std::uint64_t metadataBytes() const override {
    return small_.metadataBytes() + main_.metadataBytes() + ghost_.metadataBytes();
  }

  [[nodiscard]] std::size_t smallCount() const { return small_.size(); }
  [[nodiscard]] std::size_t mainCount() const { return main_.size(); }
  [[nodiscard]] std::size_t ghostCount() const { return ghost_.size(); }

 private:
  static std::size_t ghostCapacity(const PolicyConfig& config) {
    const double ratio = std::clamp(config.params.getDouble("ghost-size-ratio", 0.90), 0.01, 4.0);
    const std::size_t base = config.entry_hint > 0 ? config.entry_hint : 1024;
    return std::max<std::size_t>(16, static_cast<std::size_t>(static_cast<double>(base) * ratio));
  }

  // Returns the victim, or nullptr if the object at S's tail was promoted
  // into M instead of leaving the cache.
  internal::CacheEntry* evictFromSmall() {
    internal::CacheEntry* candidate = small_.back();
    if (candidate == nullptr) return nullptr;
    const std::uint8_t freq = small_.payload(candidate);
    const std::uint64_t obj_id = candidate->objId();
    const std::uint32_t size = candidate->size();
    if (small_.popBack() == nullptr) return nullptr;

    if (freq >= promote_threshold_) {
      // Proved reuse while in S: into M, with its counter spent. It keeps no
      // credit, so it has to prove itself again to survive a lap of M.
      if (!ok(main_.pushFront(candidate))) return nullptr;
      main_.payload(candidate) = 0;
      return nullptr;
    }
    // Never reused in S. Out, but remembered: if it comes back we will know
    // its reuse distance is simply longer than S.
    ghost_.record(obj_id, size);
    return candidate;
  }

  // Returns the victim, or nullptr if M's tail object bought itself a lap.
  internal::CacheEntry* evictFromMain() {
    internal::CacheEntry* candidate = main_.back();
    if (candidate == nullptr) return nullptr;
    std::uint8_t& freq = main_.payload(candidate);
    if (freq > 0) {
      --freq;
      if (!ok(main_.moveToFront(candidate))) return nullptr;
      return nullptr;
    }
    return main_.popBack();
  }

  std::uint64_t capacity_bytes_;
  double small_ratio_;
  std::uint8_t promote_threshold_;
  std::uint64_t small_bytes_budget_ = 0;
  Queue<std::uint8_t> small_;
  Queue<std::uint8_t> main_;
  IdHistory ghost_;
  bool admit_to_main_ = false;
};

}  // namespace cachesim::policies
