#pragma once

#include <cstdint>
#include <string>

#include "internal/common/error.hpp"
#include "internal/datastructures/priority_queue.hpp"
#include "internal/eviction/eviction_policy.hpp"

namespace cachesim::policies {

// Belady's MIN / OPT — the offline optimal policy, and therefore the only
// honest upper bound on what any online policy could have achieved on a given
// trace.
//
// The rule is one line: always evict the object whose next request is
// furthest in the future. An object never requested again is furthest of all.
// It is optimal for a fixed-size, uniform-cost cache, and it is not
// implementable in reality because it requires knowing the future — which is
// exactly why it belongs in a simulator, where the future is sitting in the
// rest of the trace file.
//
// It needs an oracle trace: every request must carry next_access_vtime, the
// logical index at which that object is next requested. libCacheSim's
// oracleGeneral and lcs formats carry it as a column, and traceConvOracle
// computes it for traces that do not. Running Belady on a trace without it
// is a configuration error and is reported as one rather than quietly
// producing a meaningless number.
//
// The data structure is the addressable max-heap: priority is
// next_access_vtime, "never again" is kNeverAgain == INT64_MAX so it sorts to
// the top with no special case, and a hit is an O(log n) re-sift in place
// through the entry's own metadata pointer — no search for the object's
// position in the heap.
class Belady final : public IEvictionPolicy {
 public:
  explicit Belady(const PolicyConfig& config = {}) : heap_(config.entry_hint) {}

  [[nodiscard]] std::string name() const override { return "Belady"; }

  Status onAdmit(internal::CacheEntry* entry, const Request& req) override {
    requireOracle(req);
    return heap_.insert(entry, priorityOf(req));
  }

  Status onHit(internal::CacheEntry* entry, const Request& req) override {
    requireOracle(req);
    if (!heap_.contains(entry)) return Status::kFailure;
    return heap_.updatePriority(entry, priorityOf(req));
  }

  internal::CacheEntry* evict() override { return heap_.pop(); }

  Status onRemove(internal::CacheEntry* entry) override { return heap_.remove(entry); }

  Status onResize(internal::CacheEntry* entry, std::uint32_t old_size,
                  std::uint32_t new_size) override {
    (void)entry;
    heap_.adjustBytes(static_cast<std::int64_t>(new_size) - static_cast<std::int64_t>(old_size));
    return Status::kSuccess;
  }

  void clear() override { heap_.clear(); }

  [[nodiscard]] std::uint64_t metadataBytes() const override { return heap_.metadataBytes(); }

 private:
  static std::int64_t priorityOf(const Request& req) {
    return req.next_access_vtime;
  }

  void requireOracle(const Request& req) {
    if (req.hasOracle()) return;
    throw ConfigError(
        "Belady needs an oracle trace: every request must carry "
        "next_access_vtime, which this trace does not. Use an oracleGeneral "
        "or lcs trace, or convert the trace first (libCacheSim's "
        "traceConvOracle computes the column).");
  }

  // Max-heap on next_access_vtime: the root is the object used furthest in
  // the future, which is the victim.
  PriorityQueue<std::int64_t> heap_;
};

}  // namespace cachesim::policies
