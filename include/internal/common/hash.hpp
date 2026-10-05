#pragma once

#include <cstdint>
#include <cstring>
#include <string_view>

namespace cachesim {

// splitmix64's finalizer: a bijection on 64 bits with good avalanche, which
// means every input bit influences roughly half the output bits. Used wherever
// we need to turn a structured integer (an object id, a virtual address, a
// seed) into something that behaves like a random 64-bit value.
//
// Why we need it: trace object ids are almost never uniformly distributed.
// Block traces hand out sector-aligned LBAs, memory traces hand out page
// numbers with the low bits already shifted off, and key-value traces often
// hand out dense counters. Feeding those straight into a modulo or a bucket
// index clusters badly; mixing first does not.
[[nodiscard]] constexpr std::uint64_t mix64(std::uint64_t x) noexcept {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

// FNV-1a over bytes. Only used off the hot path (hashing non-numeric object
// ids out of CSV/text traces into the uint64 id space the engine works in).
// Non-cryptographic and deliberately simple; collisions merge two distinct
// trace objects, which at 64 bits is vanishingly unlikely for any real trace
// (birthday bound: ~1 in 10^9 even at 190M distinct keys).
[[nodiscard]] inline std::uint64_t hashBytes(const char* data, std::size_t len) noexcept {
  std::uint64_t h = 0xcbf29ce484222325ULL;
  for (std::size_t i = 0; i < len; ++i) {
    h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(data[i]));
    h *= 0x100000001b3ULL;
  }
  return h;
}

[[nodiscard]] inline std::uint64_t hashBytes(std::string_view s) noexcept {
  return hashBytes(s.data(), s.size());
}

// xoshiro256++ — small, fast, statistically solid PRNG. We need our own
// rather than std::mt19937_64 because Random/Clock-style policies draw one
// number per eviction in the hot loop, and because a simulation must be
// bit-for-bit reproducible from its seed regardless of standard library
// version (std:: distributions are not specified to be portable).
class Rng {
 public:
  explicit Rng(std::uint64_t seed = 0x243f6a8885a308d3ULL) noexcept {
    // SplitMix64 is the author-recommended way to spread a single seed over
    // xoshiro's 256 bits of state; seeding with mostly-zero state would make
    // the first outputs poor.
    for (std::uint64_t& s : s_) {
      seed += 0x9e3779b97f4a7c15ULL;
      std::uint64_t z = seed;
      z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
      z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
      s = z ^ (z >> 31);
    }
  }

  std::uint64_t next() noexcept {
    const std::uint64_t result = rotl(s_[0] + s_[3], 23) + s_[0];
    const std::uint64_t t = s_[1] << 17;
    s_[2] ^= s_[0];
    s_[3] ^= s_[1];
    s_[1] ^= s_[2];
    s_[0] ^= s_[3];
    s_[2] ^= t;
    s_[3] = rotl(s_[3], 45);
    return result;
  }

  // Uniform in [0, bound) via Lemire's multiply-shift. One multiply in the
  // common case, no division, and unbiased after the (rarely taken) rejection
  // branch.
  std::uint64_t below(std::uint64_t bound) noexcept {
    if (bound <= 1) return 0;
    const auto threshold = static_cast<std::uint64_t>(-bound) % bound;
    for (;;) {
      const std::uint64_t r = next();
      if (r >= threshold) return mulhi(r, bound);
    }
  }

  // Uniform in [0, 1).
  double nextDouble() noexcept {
    return static_cast<double>(next() >> 11) * 0x1.0p-53;
  }

 private:
  static std::uint64_t rotl(std::uint64_t x, int k) noexcept {
    return (x << k) | (x >> (64 - k));
  }
  static std::uint64_t mulhi(std::uint64_t a, std::uint64_t b) noexcept {
#if defined(__SIZEOF_INT128__)
    // __int128 is a compiler extension, so -Wpedantic objects to naming it
    // even where it exists. The fallback below is correct but slower, and
    // this multiply is on the eviction path of every sampling policy.
    __extension__ using u128 = unsigned __int128;
    return static_cast<std::uint64_t>((static_cast<u128>(a) * b) >> 64);
#else
    // Portable 64x64 -> high 64 fallback for compilers without __int128.
    const std::uint64_t a_lo = a & 0xffffffffULL, a_hi = a >> 32;
    const std::uint64_t b_lo = b & 0xffffffffULL, b_hi = b >> 32;
    const std::uint64_t lo_lo = a_lo * b_lo;
    const std::uint64_t mid1 = a_hi * b_lo + (lo_lo >> 32);
    const std::uint64_t mid2 = a_lo * b_hi + (mid1 & 0xffffffffULL);
    return a_hi * b_hi + (mid1 >> 32) + (mid2 >> 32);
#endif
  }

  std::uint64_t s_[4]{};
};

}  // namespace cachesim
