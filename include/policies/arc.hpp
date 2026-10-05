#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

#include "internal/datastructures/id_history.hpp"
#include "internal/datastructures/queue.hpp"
#include "internal/eviction/eviction_policy.hpp"

namespace cachesim::policies {

// ARC — Adaptive Replacement Cache (Megiddo & Modha, FAST'03).
//
// ARC keeps two LRU lists of cached objects and two ghost lists of ids:
//   T1  objects seen exactly once recently   (the recency half)
//   T2  objects seen at least twice recently (the frequency half)
//   B1  ids recently evicted from T1
//   B2  ids recently evicted from T2
//
// and one number, p: the target size of T1. Everything interesting is in how
// p moves. A miss whose id is remembered in B1 means "we threw this out of
// the recency half too early", so p grows and T1 is allowed more room. A miss
// remembered in B2 means the same about the frequency half, so p shrinks.
// The ghost lists are exactly the evidence needed to tell those two cases
// apart, and they cost only an id each.
//
// That is what makes ARC adaptive in the real sense: it does not tune a
// parameter against a trace, it moves continuously during the trace, so a
// workload that switches from scanning to looping is tracked rather than
// averaged over. It is the policy to beat, and the reason it is here.
//
// Byte capacities. Classic ARC counts objects and assumes they are all the
// same size; this cache is byte-based, so every |·| below is a byte total and
// the adaptation step is the requested object's size rather than 1. With
// uniform object sizes the two formulations are identical request for
// request, which the test suite checks directly against a reference
// implementation and against libCacheSim.
//
// p is a double, not an integer. The paper's adaptation step is |B2|/|B1| (or
// its reciprocal), which is a ratio and is almost never whole, so truncating
// it to an integer loses the fractional part of every single adaptation. The
// error does not cancel -- it is one-signed, because truncation always moves
// p towards its starting value -- so p systematically under-adapts and the
// policy drifts away from ARC. It cost about 0.3% of hit ratio here, and was
// found by comparing against libCacheSim, which also keeps p real-valued.
class Arc final : public IEvictionPolicy {
 public:
  explicit Arc(const PolicyConfig& config = {})
      : capacity_bytes_(config.capacity_bytes),
        t1_(config.entry_hint),
        t2_(config.entry_hint),
        b1_(ghostCapacity(config)),
        b2_(ghostCapacity(config)) {}

  [[nodiscard]] std::string name() const override { return "ARC"; }

  Status onMiss(const Request& req) override {
    const auto size = static_cast<std::uint64_t>(req.size);
    ghost_ = Ghost::kNone;

    if (b1_.contains(req.obj_id)) {
      // Case II: this object was pushed out of the recency half and came
      // back. Grow T1's target. The step is scaled by how much bigger B2 is
      // than B1, which makes the response proportional to how lopsided the
      // evidence currently is.
      ghost_ = Ghost::kB1;
      const double b1 = static_cast<double>(std::max<std::uint64_t>(b1_.bytes(), 1));
      const double b2 = static_cast<double>(b2_.bytes());
      const double delta = std::max(b2 / b1, 1.0) * static_cast<double>(size);
      p_ = std::min(static_cast<double>(capacity_bytes_), p_ + delta);
      b1_.erase(req.obj_id);
    } else if (b2_.contains(req.obj_id)) {
      // Case III: the mirror image. Shrink T1's target.
      ghost_ = Ghost::kB2;
      const double b2 = static_cast<double>(std::max<std::uint64_t>(b2_.bytes(), 1));
      const double b1 = static_cast<double>(b1_.bytes());
      const double delta = std::max(b1 / b2, 1.0) * static_cast<double>(size);
      p_ = std::max(p_ - delta, 0.0);
      b2_.erase(req.obj_id);
    } else {
      // Case IV: a genuinely new object. ARC's directory has two bounds to
      // keep: |T1| + |B1| <= c, and the whole directory <= 2c. The
      // cache-side eviction that classic ARC also performs here is not our
      // job — the engine asks for victims through evict() until the new
      // object fits, in the same order — but which ghost list gives way, and
      // whether the coming eviction is ghosted at all, is decided here.
      if (t1_.bytes() + b1_.bytes() + size > capacity_bytes_) {
        if (!b1_.empty()) {
          // There is a ghost to drop, so L1's bound is relieved by dropping
          // the oldest one.
          //
          // The paper's test here is |T1| < c. Under the invariant
          // |T1| + |B1| <= c the two are equivalent, because reaching this
          // branch means |T1| + |B1| == c exactly, so |T1| < c iff B1 is
          // non-empty. Testing B1 directly is the same thing where the
          // invariant holds and the safe thing where variable object sizes
          // make it approximate -- otherwise this branch can decide to drop a
          // ghost that is not there.
          b1_.popOldest();
        } else {
          // T1 alone already fills the cache, so B1 is empty and there is no
          // ghost to drop: L1's bound can only be relieved by shrinking T1
          // itself. The object that leaves is therefore *discarded*, not
          // ghosted — ghosting it would push |T1| + |B1| straight back to
          // the bound it was just brought under. This is the one eviction in
          // ARC that leaves no trace, and getting it wrong changes B1's
          // contents and with them every later adaptation of p.
          discard_next_victim_ = true;
        }
      } else if (directoryBytes() + size > 2 * capacity_bytes_ && !b2_.empty()) {
        b2_.popOldest();
      }
    }
    return Status::kSuccess;
  }

  Status onAdmit(internal::CacheEntry* entry, const Request& req) override {
    (void)req;
    // A ghost hit in either list means this object has now been seen twice
    // within the directory's memory, so it belongs in the frequency half.
    if (ghost_ != Ghost::kNone) {
      ghost_ = Ghost::kNone;
      return t2_.pushFront(entry);
    }
    return t1_.pushFront(entry);
  }

  Status onHit(internal::CacheEntry* entry, const Request& req) override {
    (void)req;
    // Case I: a second access promotes out of the recency half, and any
    // further access just refreshes its position in the frequency half.
    if (t1_.contains(entry)) {
      if (!ok(t1_.unlink(entry))) return Status::kFailure;
      return t2_.pushFront(entry);
    }
    if (t2_.contains(entry)) return t2_.moveToFront(entry);
    return Status::kFailure;
  }

  // ARC's REPLACE. Which half gives up an object is decided purely by T1's
  // size against its target p — with one tie-break: when the current miss
  // came from B2 and T1 is exactly at target, take from T1 anyway, so that
  // repeated B2 hits cannot stall the adaptation.
  internal::CacheEntry* evict() override {
    if (discard_next_victim_ && !t1_.empty()) {
      // Case IV with T1 filling the cache: take T1's LRU and do not ghost it
      // (see onMiss).
      discard_next_victim_ = false;
      return t1_.popBack();
    }
    discard_next_victim_ = false;

    const bool take_from_t1 =
        !t1_.empty() && (static_cast<double>(t1_.bytes()) > p_ ||
                         (ghost_ == Ghost::kB2 &&
                          static_cast<double>(t1_.bytes()) == p_));

    if (take_from_t1) {
      internal::CacheEntry* victim = t1_.back();
      if (victim != nullptr) {
        const std::uint64_t obj_id = victim->objId();
        const std::uint32_t size = victim->size();
        if (t1_.popBack() == nullptr) return nullptr;
        b1_.record(obj_id, size);
        return victim;
      }
    }
    if (internal::CacheEntry* victim = t2_.back(); victim != nullptr) {
      const std::uint64_t obj_id = victim->objId();
      const std::uint32_t size = victim->size();
      if (t2_.popBack() == nullptr) return nullptr;
      b2_.record(obj_id, size);
      return victim;
    }
    // T2 empty: fall back to T1 regardless of p.
    if (internal::CacheEntry* victim = t1_.back(); victim != nullptr) {
      const std::uint64_t obj_id = victim->objId();
      const std::uint32_t size = victim->size();
      if (t1_.popBack() == nullptr) return nullptr;
      b1_.record(obj_id, size);
      return victim;
    }
    return nullptr;
  }

  Status onRemove(internal::CacheEntry* entry) override {
    if (t1_.contains(entry)) return t1_.unlink(entry);
    if (t2_.contains(entry)) return t2_.unlink(entry);
    return Status::kFailure;
  }

  Status onResize(internal::CacheEntry* entry, std::uint32_t old_size,
                  std::uint32_t new_size) override {
    const auto delta =
        static_cast<std::int64_t>(new_size) - static_cast<std::int64_t>(old_size);
    if (t1_.contains(entry)) {
      t1_.adjustBytes(delta);
      return Status::kSuccess;
    }
    if (t2_.contains(entry)) {
      t2_.adjustBytes(delta);
      return Status::kSuccess;
    }
    return Status::kFailure;
  }

  void clear() override {
    t1_.clear();
    t2_.clear();
    b1_.clear();
    b2_.clear();
    p_ = 0.0;
    ghost_ = Ghost::kNone;
    discard_next_victim_ = false;
  }

  [[nodiscard]] std::uint64_t metadataBytes() const override {
    return t1_.metadataBytes() + t2_.metadataBytes() + b1_.metadataBytes() +
           b2_.metadataBytes();
  }

  // Exposed for the tests, which check p against a reference implementation.
  [[nodiscard]] double target() const { return p_; }
  [[nodiscard]] std::size_t t1Count() const { return t1_.size(); }
  [[nodiscard]] std::size_t t2Count() const { return t2_.size(); }
  [[nodiscard]] std::size_t b1Count() const { return b1_.size(); }
  [[nodiscard]] std::size_t b2Count() const { return b2_.size(); }

 private:
  enum class Ghost : std::uint8_t { kNone, kB1, kB2 };

  // Each ghost list holds at most c ids, so sizing it to the cache's expected
  // object count is the right bound.
  static std::size_t ghostCapacity(const PolicyConfig& config) {
    return std::max<std::size_t>(16, config.entry_hint > 0 ? config.entry_hint : 1024);
  }

  [[nodiscard]] std::uint64_t directoryBytes() const {
    return t1_.bytes() + t2_.bytes() + b1_.bytes() + b2_.bytes();
  }

  std::uint64_t capacity_bytes_;
  Queue<> t1_;
  Queue<> t2_;
  IdHistory b1_;
  IdHistory b2_;
  // Target byte size of T1, real-valued (see the note at the top of the
  // class). Starts at 0, which means "assume frequency matters and let
  // recency earn its space".
  double p_ = 0.0;
  // Which ghost list the current miss was found in. Set in onMiss, consumed
  // by evict() (for the tie-break) and onAdmit (for the target list).
  Ghost ghost_ = Ghost::kNone;
  // Set in onMiss when the next eviction must come out of T1 un-ghosted.
  bool discard_next_victim_ = false;
};

}  // namespace cachesim::policies
