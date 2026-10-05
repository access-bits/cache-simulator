#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "internal/common/string_util.hpp"
#include "internal/datastructures/queue.hpp"
#include "internal/eviction/eviction_policy.hpp"

namespace cachesim::policies {

// Segmented LRU: N LRU lists stacked lowest-to-highest, each holding a fixed
// share of the capacity.
//
// A new object enters the bottom segment. Each subsequent hit promotes it one
// segment up. When a segment overflows its share, its least recently used
// object is demoted into the segment below; the bottom segment's overflow is
// a real eviction.
//
// The point is scan resistance. Plain LRU gives a single-use object the same
// prime position as the hottest object in the cache, so a scan of N distinct
// objects flushes the N most valuable things. Here a single-use object never
// leaves the bottom segment, so a scan can only ever flush that segment's
// share — with four equal segments, 25% of the cache instead of all of it,
// and the working set in the upper segments survives untouched.
//
// Parameters:
//   segments=N (default 4; "S4LRU" is this policy with N=4)
//   fractions=a|b|c|d — share of capacity per segment, bottom first. Must
//     have N entries; they are normalized, so "1|1|1|1" and "25|25|25|25"
//     are the same thing. Default is equal shares.
class Slru final : public IEvictionPolicy {
 public:
  explicit Slru(const PolicyConfig& config = {})
      : capacity_bytes_(config.capacity_bytes) {
    const auto requested = config.params.getInt("segments", 4);
    const std::size_t count =
        static_cast<std::size_t>(std::clamp<std::int64_t>(requested, 1, 16));

    std::vector<double> weights(count, 1.0);
    const std::string fractions = config.params.getString("fractions");
    if (!fractions.empty()) {
      const auto parts = split(fractions, '|');
      if (parts.size() == count) {
        for (std::size_t i = 0; i < count; ++i) {
          const auto value = parseDouble(parts[i]);
          weights[i] = value && *value > 0 ? *value : 0.0;
        }
      }
    }
    double total = 0;
    for (const double w : weights) total += w;
    if (total <= 0) {
      weights.assign(count, 1.0);
      total = static_cast<double>(count);
    }

    const std::size_t per_segment_hint =
        config.entry_hint > 0 ? std::max<std::size_t>(1, config.entry_hint / count) : 0;
    segments_.reserve(count);
    budgets_.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      segments_.push_back(std::make_unique<Segment>(per_segment_hint,
                                                    static_cast<std::uint8_t>(i)));
      budgets_.push_back(static_cast<std::uint64_t>(
          static_cast<double>(capacity_bytes_) * weights[i] / total));
    }
  }

  [[nodiscard]] std::string name() const override {
    return "SLRU-" + std::to_string(segments_.size());
  }

  Status onAdmit(internal::CacheEntry* entry, const Request& req) override {
    (void)req;
    const Status status = segments_[0]->pushFront(entry);
    if (!ok(status)) return status;
    segments_[0]->payload(entry) = 0;
    rebalance();
    return Status::kSuccess;
  }

  Status onHit(internal::CacheEntry* entry, const Request& req) override {
    (void)req;
    const std::size_t level = levelOf(entry);
    if (level == kNotFound) return Status::kFailure;
    if (level + 1 == segments_.size()) {
      // Already in the top segment: ordinary LRU promotion within it.
      return segments_[level]->moveToFront(entry);
    }
    if (!ok(segments_[level]->unlink(entry))) return Status::kFailure;
    const std::size_t target = level + 1;
    const Status status = segments_[target]->pushFront(entry);
    if (!ok(status)) return status;
    segments_[target]->payload(entry) = static_cast<std::uint8_t>(target);
    // Promotion can push the upper segment over its share, which cascades
    // downwards as demotions. No object leaves the cache here — only the
    // bottom segment's overflow does that, and the engine asks for it.
    rebalance();
    return Status::kSuccess;
  }

  internal::CacheEntry* evict() override {
    // Lowest non-empty segment first: that is where the least valuable
    // objects are by construction.
    for (auto& segment : segments_) {
      if (internal::CacheEntry* victim = segment->popBack(); victim != nullptr) {
        return victim;
      }
    }
    return nullptr;
  }

  Status onRemove(internal::CacheEntry* entry) override {
    const std::size_t level = levelOf(entry);
    if (level == kNotFound) return Status::kFailure;
    return segments_[level]->unlink(entry);
  }

  Status onResize(internal::CacheEntry* entry, std::uint32_t old_size,
                  std::uint32_t new_size) override {
    const std::size_t level = levelOf(entry);
    if (level == kNotFound) return Status::kFailure;
    segments_[level]->adjustBytes(static_cast<std::int64_t>(new_size) -
                                  static_cast<std::int64_t>(old_size));
    rebalance();
    return Status::kSuccess;
  }

  void clear() override {
    for (auto& segment : segments_) segment->clear();
  }

  [[nodiscard]] std::uint64_t metadataBytes() const override {
    std::uint64_t total = 0;
    for (const auto& segment : segments_) total += segment->metadataBytes();
    return total;
  }

 private:
  // Payload is the object's segment index, Tag is the segment's own index.
  using Segment = Queue<std::uint8_t, std::uint8_t>;
  static constexpr std::size_t kNotFound = ~std::size_t{0};

  std::size_t levelOf(internal::CacheEntry* entry) const {
    if (entry == nullptr || entry->metadata == nullptr) return kNotFound;
    const std::size_t level = Segment::payloadOf(entry);
    if (level >= segments_.size() || !segments_[level]->contains(entry)) return kNotFound;
    return level;
  }

  // Pushes each over-budget segment's overflow down one level. Only runs from
  // the top down, so one pass is enough: an object demoted into level i is
  // accounted for before level i is examined.
  void rebalance() {
    for (std::size_t level = segments_.size(); level-- > 1;) {
      Segment& from = *segments_[level];
      Segment& to = *segments_[level - 1];
      while (from.bytes() > budgets_[level]) {
        internal::CacheEntry* demoted = from.popBack();
        if (demoted == nullptr) break;
        if (!ok(to.pushFront(demoted))) return;
        to.payload(demoted) = static_cast<std::uint8_t>(level - 1);
      }
    }
  }

  std::vector<std::unique_ptr<Segment>> segments_;  // index 0 is the bottom
  std::vector<std::uint64_t> budgets_;
  std::uint64_t capacity_bytes_ = 0;
};

}  // namespace cachesim::policies
