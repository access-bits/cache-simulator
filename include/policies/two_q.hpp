#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

#include "internal/common/string_util.hpp"
#include "internal/datastructures/id_history.hpp"
#include "internal/datastructures/queue.hpp"
#include "internal/eviction/eviction_policy.hpp"

namespace cachesim::policies {

// 2Q (Johnson & Shasha, VLDB'94) — the first widely used policy to separate
// "seen once" from "seen twice", and still a strong baseline.
//
// Three structures:
//   A1in  a small FIFO (default 25% of capacity) where every newly admitted
//         object lands. A hit here does nothing at all — deliberately: the
//         object has not yet proved it has reuse *beyond* the burst it
//         arrived in.
//   A1out a ghost FIFO of ids (default 50% of capacity worth) evicted from
//         A1in. No data, just the memory that we saw this id recently.
//   Am    an LRU list of objects that missed while their id was in A1out —
//         that is, objects whose reuse interval is longer than A1in but
//         shorter than A1out. These are the real working set.
//
// What this buys over LRU: a scan of unique objects passes through A1in and
// leaves, touching Am not at all, so the working set is never flushed. What
// it buys over a plain two-segment scheme: the ghost list means the decision
// to promote is made on *evidence of reuse at a useful distance*, not on a
// second hit that might just be part of the same burst.
class TwoQ final : public IEvictionPolicy {
 public:
  explicit TwoQ(const PolicyConfig& config = {})
      : capacity_bytes_(config.capacity_bytes),
        kin_fraction_(std::clamp(config.params.getDouble("kin", 0.25), 0.01, 0.99)),
        kout_fraction_(std::clamp(config.params.getDouble("kout", 0.5), 0.01, 4.0)),
        a1in_(config.entry_hint / 4),
        am_(config.entry_hint),
        a1out_(ghostCapacity(config)) {
    kin_bytes_ = static_cast<std::uint64_t>(
        static_cast<double>(capacity_bytes_) * kin_fraction_);
  }

  [[nodiscard]] std::string name() const override { return "2Q"; }

  Status onMiss(const Request& req) override {
    // Remembered from A1out: this object's reuse distance is long enough to
    // be worth a slot in Am.
    promote_on_admit_ = a1out_.erase(req.obj_id);
    return Status::kSuccess;
  }

  Status onAdmit(internal::CacheEntry* entry, const Request& req) override {
    (void)req;
    if (promote_on_admit_) {
      promote_on_admit_ = false;
      return am_.pushFront(entry);
    }
    return a1in_.pushFront(entry);
  }

  Status onHit(internal::CacheEntry* entry, const Request& req) override {
    (void)req;
    if (am_.contains(entry)) return am_.moveToFront(entry);
    // A hit in A1in is intentionally ignored: promotion is earned by missing
    // while in A1out, not by being hit again straight away.
    if (a1in_.contains(entry)) return Status::kSuccess;
    return Status::kFailure;
  }

  internal::CacheEntry* evict() override {
    if (a1in_.bytes() > kin_bytes_ || am_.empty()) {
      if (internal::CacheEntry* victim = a1in_.back(); victim != nullptr) {
        const std::uint64_t obj_id = victim->objId();
        const std::uint32_t size = victim->size();
        if (a1in_.popBack() == nullptr) return nullptr;
        a1out_.record(obj_id, size);
        return victim;
      }
    }
    // Objects leaving Am are not ghosted: they already had their chance in
    // Am, and remembering them would let a cold object cycle straight back.
    if (internal::CacheEntry* victim = am_.popBack(); victim != nullptr) return victim;
    return a1in_.popBack();
  }

  Status onRemove(internal::CacheEntry* entry) override {
    if (a1in_.contains(entry)) return a1in_.unlink(entry);
    if (am_.contains(entry)) return am_.unlink(entry);
    return Status::kFailure;
  }

  Status onResize(internal::CacheEntry* entry, std::uint32_t old_size,
                  std::uint32_t new_size) override {
    const auto delta =
        static_cast<std::int64_t>(new_size) - static_cast<std::int64_t>(old_size);
    if (a1in_.contains(entry)) {
      a1in_.adjustBytes(delta);
      return Status::kSuccess;
    }
    if (am_.contains(entry)) {
      am_.adjustBytes(delta);
      return Status::kSuccess;
    }
    return Status::kFailure;
  }

  void clear() override {
    a1in_.clear();
    am_.clear();
    a1out_.clear();
    promote_on_admit_ = false;
  }

  [[nodiscard]] std::uint64_t metadataBytes() const override {
    return a1in_.metadataBytes() + am_.metadataBytes() + a1out_.metadataBytes();
  }

 private:
  // The ghost list is bounded in *entries*, not bytes, so it needs an object
  // count. entry_hint is the cache's expected object count; kout is a
  // multiple of that.
  static std::size_t ghostCapacity(const PolicyConfig& config) {
    const double kout = std::clamp(config.params.getDouble("kout", 0.5), 0.01, 4.0);
    const std::size_t base = config.entry_hint > 0 ? config.entry_hint : 1024;
    return std::max<std::size_t>(16, static_cast<std::size_t>(static_cast<double>(base) * kout));
  }

  std::uint64_t capacity_bytes_;
  double kin_fraction_;
  double kout_fraction_;
  std::uint64_t kin_bytes_ = 0;
  Queue<> a1in_;
  Queue<> am_;
  IdHistory a1out_;
  bool promote_on_admit_ = false;
};

}  // namespace cachesim::policies
