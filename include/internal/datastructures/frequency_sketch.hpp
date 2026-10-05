#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "internal/common/hash.hpp"

namespace cachesim {

// A count-min sketch with 4-bit saturating counters and periodic halving —
// the frequency estimator TinyLFU is built on.
//
// The problem it solves: LFU needs to know how often an object is used, but
// keeping an exact counter per object means keeping state for objects that
// are not even cached (otherwise a one-hit object admitted now looks exactly
// like a popular object admitted now), and that state grows with the whole
// trace rather than with the cache. The sketch instead keeps a fixed amount
// of memory — four rows of 4-bit counters, 2 bytes per cache slot in total —
// and answers "roughly how hot is this id" with a bounded over-estimate.
//
// Two properties make it work:
//   * min over rows. Each row hashes the id to one counter and counts
//     collisions too, so each row over-estimates; taking the smallest of four
//     independent over-estimates makes a large error unlikely.
//   * halving. Every counter is halved once the sketch has absorbed
//     sample_size increments, so the estimate tracks *recent* popularity
//     rather than all-time popularity. Without it a sketch converges to the
//     trace's lifetime distribution and stops adapting, which is the classic
//     LFU failure mode (an object popular an hour ago blocking an object
//     popular now).
//
// Counters saturate at 15: once an object is clearly hot, how hot stops
// mattering, and the cap is what keeps the counters 4 bits.
class FrequencySketch {
 public:
  // expected_entries: the cache's object capacity. The sketch is sized from
  //   the cache, not from the trace — that is what makes its memory bounded.
  //
  //   Each row gets four counters per cache entry, so sixteen counters in
  //   total per entry, which at half a byte each is eight bytes per cache
  //   slot. That ratio is Caffeine's and it is not a free parameter: with
  //   only one counter per entry per row, a trace whose distinct-object count
  //   is far above the cache size (which is every interesting trace) collides
  //   so heavily that every id's estimate saturates, every admission
  //   comparison is a tie, and the filter stops filtering. The symptom is a
  //   W-TinyLFU that scores exactly LRU's hit ratio and does not respond to
  //   any of its parameters.
  explicit FrequencySketch(std::size_t expected_entries,
                           std::uint64_t seed = 0x243f6a8885a308d3ULL) {
    std::size_t width = 1;
    while (width < expected_entries) width <<= 1;
    width *= kCountersPerEntry;
    if (width < 64) width = 64;
    width_mask_ = width - 1;
    // Two counters per byte, four rows.
    rows_.assign(kRows, std::vector<std::uint8_t>((width + 1) / 2, 0));
    // Reset after ten increments per cache *slot* (not per counter): often
    // enough to track a phase change within a few cache-fulls of traffic,
    // rare enough that the halving pass costs nothing measurable.
    sample_size_ = 10 * std::max<std::size_t>(expected_entries, 16);
    for (std::size_t r = 0; r < kRows; ++r) {
      seeds_[r] = mix64(seed + 0x9e3779b97f4a7c15ULL * (r + 1));
    }
  }

  // Estimated recent access count of obj_id, in [0, 15].
  [[nodiscard]] std::uint8_t estimate(std::uint64_t obj_id) const {
    std::uint8_t min_count = 15;
    for (std::size_t r = 0; r < kRows; ++r) {
      const std::uint8_t count = read(r, indexFor(r, obj_id));
      if (count < min_count) min_count = count;
    }
    return min_count;
  }

  void increment(std::uint64_t obj_id) {
    bool incremented = false;
    for (std::size_t r = 0; r < kRows; ++r) {
      const std::size_t index = indexFor(r, obj_id);
      const std::uint8_t count = read(r, index);
      if (count < 15) {
        write(r, index, static_cast<std::uint8_t>(count + 1));
        incremented = true;
      }
    }
    // Only a real increment counts towards the reset budget: an id whose
    // counters are all saturated adds no information, and letting it drive
    // resets would halve the whole sketch on the strength of one hot object.
    if (incremented && ++additions_ >= sample_size_) halve();
  }

  void clear() {
    for (auto& row : rows_) {
      std::fill(row.begin(), row.end(), std::uint8_t{0});
    }
    additions_ = 0;
  }

  [[nodiscard]] std::uint64_t metadataBytes() const {
    return static_cast<std::uint64_t>(kRows) * rows_[0].capacity();
  }

 private:
  static constexpr std::size_t kRows = 4;
  static constexpr std::size_t kCountersPerEntry = 4;

  [[nodiscard]] std::size_t indexFor(std::size_t row, std::uint64_t obj_id) const {
    return static_cast<std::size_t>(mix64(obj_id ^ seeds_[row])) & width_mask_;
  }

  [[nodiscard]] std::uint8_t read(std::size_t row, std::size_t index) const {
    const std::uint8_t byte = rows_[row][index >> 1];
    return (index & 1) != 0 ? static_cast<std::uint8_t>(byte >> 4)
                            : static_cast<std::uint8_t>(byte & 0x0f);
  }

  void write(std::size_t row, std::size_t index, std::uint8_t value) {
    std::uint8_t& byte = rows_[row][index >> 1];
    if ((index & 1) != 0) {
      byte = static_cast<std::uint8_t>((byte & 0x0f) | (value << 4));
    } else {
      byte = static_cast<std::uint8_t>((byte & 0xf0) | (value & 0x0f));
    }
  }

  void halve() {
    for (auto& row : rows_) {
      for (std::uint8_t& byte : row) {
        // Both nibbles shift right one place at once; the 0x77 mask stops
        // the high nibble's low bit bleeding into the low nibble's high bit.
        byte = static_cast<std::uint8_t>((byte >> 1) & 0x77);
      }
    }
    additions_ = 0;
  }

  std::vector<std::vector<std::uint8_t>> rows_;
  std::uint64_t seeds_[kRows]{};
  std::size_t width_mask_ = 0;
  std::size_t sample_size_ = 0;
  std::size_t additions_ = 0;
};

}  // namespace cachesim
