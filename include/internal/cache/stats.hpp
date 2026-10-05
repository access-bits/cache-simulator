#pragma once

#include <cstdint>

namespace cachesim {

// Counters for one simulated cache.
//
// Everything here is a plain counter; every ratio is derived on demand. That
// way a warm-up phase can be discarded by zeroing the struct, and two
// partial runs can be summed, without any derived quantity going stale.
struct Stats {
  std::uint64_t n_req = 0;
  std::uint64_t n_hit = 0;
  std::uint64_t n_req_byte = 0;
  std::uint64_t n_hit_byte = 0;

  // Objects admitted and evicted over the run. n_admit - n_evict is the
  // number of objects still resident (modulo explicit removals).
  std::uint64_t n_admit = 0;
  std::uint64_t n_evict = 0;
  std::uint64_t n_evict_byte = 0;

  // Requests for objects larger than the entire cache. They are counted as
  // misses (they are), but they can never be admitted, so a run with a large
  // count here is really telling you the capacity is below the object size
  // distribution.
  std::uint64_t n_oversized = 0;

  // Requests dropped before the cache saw them, by a filter plugin (a TLB
  // absorbing a hit, a PEBS sampler dropping an unsampled access). Not
  // counted in n_req: the cache genuinely never saw them.
  std::uint64_t n_filtered = 0;

  [[nodiscard]] std::uint64_t nMiss() const { return n_req - n_hit; }
  [[nodiscard]] std::uint64_t nMissByte() const { return n_req_byte - n_hit_byte; }

  [[nodiscard]] double missRatio() const {
    return n_req > 0 ? static_cast<double>(nMiss()) / static_cast<double>(n_req) : 0.0;
  }
  [[nodiscard]] double hitRatio() const {
    return n_req > 0 ? static_cast<double>(n_hit) / static_cast<double>(n_req) : 0.0;
  }
  [[nodiscard]] double byteMissRatio() const {
    return n_req_byte > 0 ? static_cast<double>(nMissByte()) / static_cast<double>(n_req_byte)
                          : 0.0;
  }

  void reset() { *this = Stats{}; }

  Stats& operator+=(const Stats& other) {
    n_req += other.n_req;
    n_hit += other.n_hit;
    n_req_byte += other.n_req_byte;
    n_hit_byte += other.n_hit_byte;
    n_admit += other.n_admit;
    n_evict += other.n_evict;
    n_evict_byte += other.n_evict_byte;
    n_oversized += other.n_oversized;
    n_filtered += other.n_filtered;
    return *this;
  }
};

}  // namespace cachesim
