#pragma once

#include <cstddef>

// Portability shims for the few compiler-specific knobs the hot path needs.
// Everything here degrades to a no-op on compilers that don't support it, so
// no translation unit has to care which compiler it is being built with.

#if defined(__GNUC__) || defined(__clang__)
#define CACHESIM_ALWAYS_INLINE inline __attribute__((always_inline))
#define CACHESIM_NOINLINE __attribute__((noinline))
#define CACHESIM_HOT __attribute__((hot))
#define CACHESIM_LIKELY(x) __builtin_expect(!!(x), 1)
#define CACHESIM_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define CACHESIM_PREFETCH(addr) __builtin_prefetch(addr)
#define CACHESIM_UNREACHABLE() __builtin_unreachable()
#else
#define CACHESIM_ALWAYS_INLINE inline
#define CACHESIM_NOINLINE
#define CACHESIM_HOT
#define CACHESIM_LIKELY(x) (x)
#define CACHESIM_UNLIKELY(x) (x)
#define CACHESIM_PREFETCH(addr) ((void)(addr))
#define CACHESIM_UNREACHABLE() ((void)0)
#endif

namespace cachesim {

// Used to pad hot per-thread state so that independent simulations never
// share a cache line — false sharing would otherwise serialize parameter
// sweep workers that have no logical interaction at all.
inline constexpr std::size_t kCacheLineSize = 64;

}  // namespace cachesim
