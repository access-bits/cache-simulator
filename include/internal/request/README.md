# `include/internal/request/`

`Request` — one trace event. 32 bytes, trivially copyable, two per cache line,
no padding holes.

```cpp
struct Request {
  std::uint64_t obj_id = 0;                      // identity
  std::int64_t  clock_time = 0;                  // wall-clock time from the trace
  std::int64_t  next_access_vtime = kNoOracle;    // the offline oracle
  std::uint32_t size = 1;                        // bytes
  std::uint16_t cpu_id = 0;                      // multi-CPU memory traces
  Op            op = Op::kUnknown;
  std::uint8_t  flags = 0;                       // reserved
};
```

## The rule that keeps this small

libCacheSim's `request_t` is the cautionary example. Every subsystem that was
ever added — admission, prefetch, the trace analyzer, multi-CPU traces — grew
its own fields directly on it, so it ended up carrying `obj_cost`, `ttl`,
`tenant_id`, `kv.{key_size,val_size}`, `n_features` with a 16-slot feature
array, `hv`, `vtime_since_last_access`, `rtime_since_last_access`,
`prev_size`, `create_rtime`, `compulsory_miss`, `first_seen_in_window`, and an
untyped `void*` escape hatch. Most of it is dead on any given run, and all of
it is in the hot loop's cache footprint.

The rule here: **a field earns its place only if a trace reader can produce it
*and* more than one consumer needs it.**

Three fields beyond the bare `(id, size, time)` triple pass that test:

- **`next_access_vtime`** — the logical index at which this object is next
  requested, `kNeverAgain` if never, `kNoOracle` if the trace does not say.
  Belady and its variants cannot exist without it, and it is a first-class
  column of libCacheSim's `oracleGeneral` and `lcs` formats, so the reader has
  it in hand already.
- **`cpu_id`** — which hardware thread issued the access. Carried by merged
  multi-CPU memory traces and needed by anything modelling per-core structures.
- **`op`** — read/write/get/set/delete. A reader that drops it cannot get it
  back, and some studies need it (write amplification, or treating a delete as
  an invalidation).

TTL, tenant, cost and ML feature vectors do not pass, and are not here.

## Why not a side channel

The alternative considered was keeping these in a parallel array keyed by
request index, leaving `Request` at 16 bytes. It was rejected because it costs
a second random memory stream in the hot loop and has to be threaded through
every reader, every policy and every plugin — two real costs to save 16 bytes
on a struct that is streamed sequentially, where the prefetcher already hides
the difference.

## `next_access_vtime`'s two sentinels

```cpp
inline constexpr std::int64_t kNeverAgain = INT64_MAX;   // never requested again
inline constexpr std::int64_t kNoOracle   = -1;          // the trace does not say
```

Keeping them distinct matters. `kNeverAgain` sorts above every real future
time, so Belady's max-heap treats such an object as the best possible victim
with no special case in the comparison. `kNoOracle` is a *different* fact, and
conflating the two would make Belady on a non-oracle trace behave as if every
object were dead — producing a confident, meaningless number instead of the
`ConfigError` it should produce. Readers normalize the format's own sentinel
(libCacheSim writes `-1` in some formats and `INT64_MAX` in others) onto
`kNeverAgain`.
