#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

#include "internal/datastructures/frequency_sketch.hpp"
#include "internal/datastructures/queue.hpp"
#include "internal/eviction/eviction_policy.hpp"

namespace cachesim::policies {

// W-TinyLFU (Einziger, Friedman & Manes) — the policy behind Caffeine, and
// the clearest example of admission control doing the work that eviction
// policies usually try to do.
//
// The core idea is that the expensive question is not "what should leave" but
// "should this even come in". An LFU admission filter answers that well but
// needs frequency statistics for objects that are not cached, which is
// unbounded state — so the frequencies come from a count-min sketch with
// 4-bit counters and periodic halving (TinyLFU), which costs about two bytes
// per cache slot and tracks recent rather than all-time popularity.
//
// Three lists hold the objects:
//   window     a small LRU (default 1% of capacity). Everything is admitted
//              here first, no questions asked, which is what lets a sudden
//              burst of new-but-genuinely-hot objects get in at all — pure
//              TinyLFU admission is famously bad at exactly that.
//   probation  the main region's entry list (20% of main), LRU.
//   protected  the main region's promoted list (80% of main), LRU.
//
// A candidate leaving the window does not go straight into the main region:
// its sketch frequency is compared against the frequency of the main
// region's own next victim, and the loser is the one that leaves the cache.
// That is the admission filter, and it is why a one-hit-wonder cannot
// displace an established object however recently it arrived.
//
// Parameters:
//   window-ratio=f    (default 0.01) window's share of capacity
//   protected-ratio=f (default 0.80) protected's share of the main region
class WTinyLfu final : public IEvictionPolicy {
 public:
  explicit WTinyLfu(const PolicyConfig& config = {})
      : capacity_bytes_(config.capacity_bytes),
        window_(std::max<std::size_t>(16, config.entry_hint / 100)),
        probation_(config.entry_hint / 4),
        protected_(config.entry_hint),
        sketch_(config.entry_hint > 0 ? config.entry_hint : 1024, config.seed) {
    const double window_ratio =
        std::clamp(config.params.getDouble("window-ratio", 0.01), 0.0, 0.9);
    const double protected_ratio =
        std::clamp(config.params.getDouble("protected-ratio", 0.80), 0.0, 1.0);
    window_budget_ =
        static_cast<std::uint64_t>(static_cast<double>(capacity_bytes_) * window_ratio);
    const std::uint64_t main_budget = capacity_bytes_ - window_budget_;
    protected_budget_ =
        static_cast<std::uint64_t>(static_cast<double>(main_budget) * protected_ratio);
  }

  [[nodiscard]] std::string name() const override { return "W-TinyLFU"; }

  Status onMiss(const Request& req) override {
    // The sketch counts *requests*, not admissions: an object that keeps
    // missing is still building up evidence that it deserves a slot. This is
    // the whole reason the sketch exists rather than a per-object counter.
    sketch_.increment(req.obj_id);
    return Status::kSuccess;
  }

  Status onAdmit(internal::CacheEntry* entry, const Request& req) override {
    (void)req;
    const Status status = window_.pushFront(entry);
    if (!ok(status)) return status;
    window_.payload(entry) = kWindow;
    drainWindow();
    return Status::kSuccess;
  }

  Status onHit(internal::CacheEntry* entry, const Request& req) override {
    sketch_.increment(req.obj_id);
    switch (regionOf(entry)) {
      case kWindow: {
        const Status status = window_.moveToFront(entry);
        // A hit can bring the object back at a larger size, which can put
        // the window over its share.
        drainWindow();
        return status;
      }
      case kProbation: {
        // Proving reuse inside the main region earns promotion out of
        // probation, where it is no longer the first thing considered for
        // eviction.
        if (!ok(probation_.unlink(entry))) return Status::kFailure;
        const Status status = protected_.pushFront(entry);
        if (!ok(status)) return status;
        protected_.payload(entry) = kProtected;
        demoteOverflow();
        return Status::kSuccess;
      }
      case kProtected:
        return protected_.moveToFront(entry);
      default:
        return Status::kFailure;
    }
  }

  internal::CacheEntry* evict() override {
    // Protected is capacity-bounded on its own; its overflow falls back into
    // probation rather than leaving the cache.
    demoteOverflow();

    // The admission contest: probation's newest entry — the most recent
    // graduate of the window — against probation's oldest. The loser is what
    // leaves the cache.
    //
    // This is the whole policy. A one-hit-wonder arrives with a sketch
    // estimate of 1 and is compared against whatever the main region was
    // about to evict; it only gets to stay if it is genuinely hotter than
    // that. On a trace dominated by objects seen once, the candidate loses
    // almost every time, so the main region is never disturbed by the churn
    // at all — which is why a scan does not flush the working set here.
    internal::CacheEntry* candidate = probation_.front();
    internal::CacheEntry* victim = probation_.back();

    if (candidate != nullptr && victim != nullptr && candidate != victim) {
      if (sketch_.estimate(candidate->objId()) > sketch_.estimate(victim->objId())) {
        return probation_.popBack();
      }
      return probation_.popFront();
    }

    if (victim != nullptr) {
      // Probation holds a single entry, so there is nothing inside probation
      // to weigh it against; the other half of the main region decides.
      internal::CacheEntry* promoted_victim = protected_.back();
      if (promoted_victim == nullptr) return probation_.popBack();
      if (sketch_.estimate(victim->objId()) > sketch_.estimate(promoted_victim->objId())) {
        return protected_.popBack();
      }
      return probation_.popBack();
    }

    if (internal::CacheEntry* evicted = protected_.popBack(); evicted != nullptr) {
      return evicted;
    }
    return window_.popBack();
  }

  Status onRemove(internal::CacheEntry* entry) override {
    switch (regionOf(entry)) {
      case kWindow: return window_.unlink(entry);
      case kProbation: return probation_.unlink(entry);
      case kProtected: return protected_.unlink(entry);
      default: return Status::kFailure;
    }
  }

  Status onResize(internal::CacheEntry* entry, std::uint32_t old_size,
                  std::uint32_t new_size) override {
    const auto delta =
        static_cast<std::int64_t>(new_size) - static_cast<std::int64_t>(old_size);
    switch (regionOf(entry)) {
      case kWindow: window_.adjustBytes(delta); return Status::kSuccess;
      case kProbation: probation_.adjustBytes(delta); return Status::kSuccess;
      case kProtected: protected_.adjustBytes(delta); return Status::kSuccess;
      default: return Status::kFailure;
    }
  }

  void clear() override {
    window_.clear();
    probation_.clear();
    protected_.clear();
    sketch_.clear();
  }

  [[nodiscard]] std::uint64_t metadataBytes() const override {
    return window_.metadataBytes() + probation_.metadataBytes() + protected_.metadataBytes() +
           sketch_.metadataBytes();
  }

 private:
  static constexpr std::uint8_t kWindow = 0;
  static constexpr std::uint8_t kProbation = 1;
  static constexpr std::uint8_t kProtected = 2;
  static constexpr std::uint8_t kNowhere = 255;

  using Region = Queue<std::uint8_t>;

  std::uint8_t regionOf(internal::CacheEntry* entry) const {
    if (entry == nullptr || entry->metadata == nullptr) return kNowhere;
    switch (Region::payloadOf(entry)) {
      case kWindow: return window_.contains(entry) ? kWindow : kNowhere;
      case kProbation: return probation_.contains(entry) ? kProbation : kNowhere;
      case kProtected: return protected_.contains(entry) ? kProtected : kNowhere;
      default: return kNowhere;
    }
  }

  // Moves everything beyond the window's share into probation, where it
  // becomes a candidate for the admission contest.
  //
  // This has to happen on admission rather than inside evict(), and the
  // reason is not obvious: the main region is only ever populated from here.
  // If the move happened during eviction instead, then the first time the
  // cache filled up, the candidate being moved in would be the main
  // region's only occupant, would therefore have nothing to be weighed
  // against, and would be evicted again immediately. The main region would
  // stay permanently empty, every eviction would come off the window's tail,
  // and the policy would quietly behave as plain LRU over one list — scoring
  // exactly LRU's hit ratio and ignoring every one of its own parameters.
  void drainWindow() {
    while (window_.bytes() > window_budget_ && !window_.empty()) {
      internal::CacheEntry* moved = window_.popBack();
      if (moved == nullptr) return;
      if (!ok(probation_.pushFront(moved))) return;
      probation_.payload(moved) = kProbation;
    }
  }

  // Protected is capacity-bounded; its overflow falls back to probation
  // rather than leaving the cache.
  void demoteOverflow() {
    while (protected_.bytes() > protected_budget_) {
      internal::CacheEntry* demoted = protected_.popBack();
      if (demoted == nullptr) return;
      if (!ok(probation_.pushFront(demoted))) return;
      probation_.payload(demoted) = kProbation;
    }
  }

  internal::CacheEntry* removeFrom(internal::CacheEntry* victim) {
    if (probation_.contains(victim)) {
      return ok(probation_.unlink(victim)) ? victim : nullptr;
    }
    if (protected_.contains(victim)) {
      return ok(protected_.unlink(victim)) ? victim : nullptr;
    }
    return nullptr;
  }

  std::uint64_t capacity_bytes_;
  std::uint64_t window_budget_ = 0;
  std::uint64_t protected_budget_ = 0;
  Region window_;
  Region probation_;
  Region protected_;
  FrequencySketch sketch_;
};

}  // namespace cachesim::policies
