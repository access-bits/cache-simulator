# cachesim

A cache simulator in C++20: replay a request trace against a cache of a given
size and eviction policy, and find out what the miss ratio would have been.

It exists because [libCacheSim](https://github.com/1a1a11a/libCacheSim) — the
usual tool for this — has four problems that are structural rather than
incidental, and all four are consequences of decisions that are hard to undo
in place:

| libCacheSim | here |
|---|---|
| A `malloc` and a `free` per cached object. On a trace with a 90% miss ratio that is an allocator round trip for almost every request. | One pre-sized arena per cache; admitting and evicting move a pointer on and off a free list. No allocation per request, anywhere. |
| A monolithic `cache_obj_t` with a union of ~20 per-policy metadata structs. Every object pays for the largest variant, and adding a policy means editing a shared header. | Each object has one pointer to whatever structure currently orders it. A policy's per-object state lives in that structure's node, so a new policy touches no shared file. |
| Parameter sweeps share one reader thread behind a batch-and-barrier queue, so every worker waits on the slowest at every batch. | Each configuration is a self-contained cache on its own thread. No locks, no barriers, no shared mutable state during replay. |
| Cross-cutting features are `if (cache->x != NULL)` checks threaded through one shared function. | Lifecycle hooks: policies implement an interface, observers are notified of evictions, and neither can be added by editing the core. |

**Status: the engine and the policies are complete and tested; the driver is
not.** See [Status](#status) below for exactly what works today.

---

## Table of contents

- [Quick start](#quick-start)
- [Status](#status)
- [Using it as a library](#using-it-as-a-library)
- [Eviction policies](#eviction-policies)
- [Architecture](#architecture)
- [Design decisions, and why](#design-decisions-and-why)
- [Trace formats](#trace-formats)
- [Testing](#testing)
- [Repository layout](#repository-layout)
- [Roadmap](#roadmap)

---

## Quick start

Requirements: a C++20 compiler (GCC 11+, Clang 14+), CMake 3.20+. Optionally
`libzstd` development headers, for reading compressed traces. Nothing is
downloaded at configure time, so this builds with no network access.

```bash
cmake -S . -B build
cmake --build build -j

ctest --test-dir build --output-on-failure   # ~181,000 assertions, under a second
./build/examples/replay                     # every policy on a synthetic trace
```

`examples/replay` builds a Zipf trace in memory and replays it against every
registered policy at four capacities. On a 4-core cloud container, 1M requests
over 100k objects:

```
policy              0.1%        1.0%        5.0%       10.0%       M req/s
--------------------------------------------------------------------------
LRU               0.7886      0.5480      0.3718      0.2931          31.2
FIFO              0.8226      0.5931      0.4139      0.3327          31.6
MRU               0.9851      0.9733      0.9167      0.8487          26.5
Random            0.8220      0.5934      0.4144      0.3326          28.4
Clock             0.7750      0.5353      0.3616      0.2844          28.2
Sieve             0.6561      0.4445      0.3078      0.2473          38.2
LFU               0.6546      0.4497      0.3079      0.2473          15.1
LFU-DA            0.6963      0.4840      0.3295      0.2617          15.4
SLRU              0.6737      0.4567      0.3094      0.2477          24.7
2Q                0.6758      0.4635      0.3192      0.2585          19.1
ARC               0.6625      0.4509      0.3079      0.2490          19.9
S3FIFO            0.6534      0.4457      0.3030      0.2442          22.7
LRU-K             0.6440      0.4485      0.3079      0.2473          20.2
W-TinyLFU         0.6597      0.4462      0.3021      0.2409          13.0
Belady            0.5924      0.3734      0.2298      0.1729          21.9
```

The ordering is a sanity check as much as a result: Belady is the offline
optimum so nothing beats it, MRU is the worst thing you can do on a
popularity-skewed trace, and the modern policies (S3FIFO, Sieve, W-TinyLFU,
LRU-K) land where the literature says they should.

**On those throughput numbers.** They are the whole hot loop — hash lookup,
policy dispatch, eviction, bookkeeping — on one thread, on a shared cloud
container, with the trace already in memory. They are useful for comparing
policies against each other in this table and for catching a regression. They
are **not** a measured comparison against libCacheSim: no such comparison has
been run yet, and this README will not claim one until it has. See
[Roadmap](#roadmap).

### Build options

| Option | Default | Effect |
|---|---|---|
| `CMAKE_BUILD_TYPE` | `Release` | A simulator built unoptimized is not one you can run a sweep with, so `Release` is the default rather than CMake's empty build type. |
| `CACHESIM_NATIVE_ARCH` | `OFF` | `-march=native`. Faster, but the binary stops being portable between machines. |
| `CACHESIM_LTO` | `ON` | Link-time optimization in `Release`. |
| `CACHESIM_WERROR` | `OFF` | Warnings become errors. |
| `CACHESIM_BUILD_TESTS` | `ON` | |
| `CACHESIM_BUILD_EXAMPLES` | `ON` | |

If zstd is not found, configuration still succeeds and says so; a `.zst` trace
then fails at open time with a message saying what to install.

---

## Status

| Component | State |
|---|---|
| Cache engine — arena, index, byte accounting, eviction sequencing, size changes, stats | **Done**, tested |
| Intrusive data structures — `Queue`, `PriorityQueue`, `IdHistory`, `RandomBag`, `FrequencySketch`, `BlockPool` | **Done**, tested |
| 15 eviction policies | **Done**; 13 cross-validated against an independent reference implementation |
| Policy registry (name → policy, for config-driven selection) | **Done** |
| Trace byte sources — `mmap`, buffered file, streaming zstd | **Done** |
| Binary record layouts — oracleGeneral, lcs v1–v8, Twitter, VSCSI v1/v2, user format strings | **Done** |
| Trace readers (`openTrace`) — CSV, text, mergedTrace, sampling, request limits | **Declared, not implemented** |
| YAML config parsing (libCacheSim-compatible schema) | Not started |
| Multi-threaded sweep runner | Not started |
| `cachesim` CLI | Not started |
| Plugin / hook layer beyond `ICacheObserver` | Not started |
| Benchmark suite, and a measured comparison against libCacheSim | Not started |

Until the CLI exists, the way to drive this is as a library — which is what
`examples/replay.cpp` shows, end to end.

---

## Using it as a library

The whole API is five steps. This is `examples/replay.cpp` with the noise
removed:

```cpp
#include "internal/cache/cache.hpp"
#include "internal/eviction/policy_registry.hpp"

using namespace cachesim;

// 1. Describe the policy. Capacity and the expected object count go here
//    because most policies size themselves from them at construction:
//    ARC's ghost lists, S3FIFO's 10/90 split, every queue's node pool.
PolicyConfig policy_config;
policy_config.capacity_bytes = 1 << 20;   // 1 MiB
policy_config.entry_hint     = 4096;      // expected peak object count
policy_config.params         = ParamMap("k=4");   // algorithm-specific options

// 2. Describe the cache.
CacheOptions options;
options.capacity_bytes = 1 << 20;
options.entry_hint     = 4096;

// 3. Build it. This is the only place a runtime string becomes a concrete
//    policy; everything after it is a single-target call.
Cache cache(options, PolicyRegistry::instance().create("LRU-K", policy_config));

// 4. Replay. One call per request; returns true on a hit.
Request req;
req.obj_id = 12345;
req.size   = 4096;
const bool hit = cache.access(req);

// 5. Read the result.
const Stats& stats = cache.stats();
printf("miss ratio %.4f, byte miss ratio %.4f\n",
       stats.missRatio(), stats.byteMissRatio());
```

### The three types you need

**`Request`** — one trace event, 32 bytes, trivially copyable.

```cpp
struct Request {
  std::uint64_t obj_id;             // identity: LBA, page number, key hash
  std::int64_t  clock_time;         // wall-clock time from the trace
  std::int64_t  next_access_vtime;  // the offline oracle; kNoOracle if absent
  std::uint32_t size;               // bytes; 1 makes capacity an object count
  std::uint16_t cpu_id;             // multi-CPU memory traces
  Op            op;                 // read/write/get/set/delete/...
  std::uint8_t  flags;
};
```

**`CacheOptions`** — how the cache behaves.

| Field | Meaning |
|---|---|
| `capacity_bytes` | Byte capacity. With every object size 1 this is simply an object count. |
| `entry_hint` | Expected peak object count. A *sizing* hint, not a cap: too low costs a few extra allocations, never correctness. |
| `max_entries` | Opt-in hard cap on resident objects; `0` (the default) means unlimited. |
| `obj_metadata_size` | Per-object bookkeeping charged against occupancy but not against request byte counts, so the byte miss ratio still refers to real traffic. |
| `update_size_on_hit` | Whether a request naming a new size for a cached object updates it. `true` matches real caches and libCacheSim. |

**`Stats`** — plain counters, with every ratio derived on demand, so a warm-up
phase can be discarded by zeroing them and two partial runs can be summed
without anything going stale.

```cpp
n_req  n_hit  n_req_byte  n_hit_byte  n_admit  n_evict  n_evict_byte
n_oversized   // requests for objects larger than the whole cache
n_filtered    // requests dropped before the cache saw them
missRatio()  hitRatio()  byteMissRatio()  nMiss()  nMissByte()
```

### Warm-up

`resetStats()` zeroes the counters and keeps the cache contents — which is
exactly what discarding a warm-up phase means. `clear()` does the opposite:
empties the cache and keeps the counters.

```cpp
for (std::size_t i = 0; i < warmup_requests; ++i) cache.access(trace[i]);
cache.resetStats();                                     // forget the warm-up
for (std::size_t i = warmup_requests; i < trace.size(); ++i) cache.access(trace[i]);
```

### Errors

Recoverable conditions inside the engine return `Status`; things a caller can
only have got wrong throw. All exceptions derive from `cachesim::Error`:
`ConfigError` (a config value you can fix), `TraceError` (a file that will not
read), `EngineError` (an invariant broken — a bug in a policy or in the
engine). Every message names the component and the state, because
`"onHit failed"` is not something you can act on eight hours into a sweep.

---

## Eviction policies

Names are matched case-insensitively with `-`, `_` and spaces ignored, so
`S3FIFO`, `s3fifo` and `s3-fifo` are the same policy. Parameters come from the
`params` string as comma-separated `key=value` pairs.

| Name | Aliases | Parameters | What it is |
|---|---|---|---|
| `LRU` | | | Least recently used. |
| `FIFO` | | | Insertion order; a hit changes nothing. The baseline that tells you whether recency information is worth anything on your trace. |
| `MRU` | | | Evicts the *most* recently used. A diagnostic: if MRU beats LRU, your trace is a scan. |
| `Random` | `rand` | | Uniform victim in O(1). The floor that says how much of a policy's hit ratio comes from its ordering rather than from the cache simply being big enough. |
| `Clock` | `second-chance`, `sc` | `n-bit-counter=K` (1) | CLOCK. A hit sets a counter instead of moving the object, so the hot path is a single byte write; the cost is paid lazily when the hand sweeps. What real OS and database caches actually implement. |
| `Sieve` | | | SIEVE (NSDI'24). Like CLOCK but the hand never *moves* an object, so surviving objects form a stable filter that new one-hit-wonders have to pass through. |
| `LFU` | | | Exact least-frequently-used, O(1) hit and O(1) amortized eviction, FIFO tie-break within a frequency. |
| `LFU-DA` | `lfuda` | | LFU plus a global age that rises to each victim's key, so the cache can forget. Fixes LFU's one serious failure mode: an object popular during a burst last week otherwise outranks everything since. |
| `SLRU` | `s4lru`, `segmented-lru` | `segments=N` (4), `fractions=a\|b\|c\|d` | N stacked LRU lists; a hit promotes one level, an overflowing level demotes into the one below. A scan can only ever flush the bottom segment's share. |
| `2Q` | `two-q` | `kin=f` (0.25), `kout=f` (0.5) | 2Q (VLDB'94). A1in (FIFO) → A1out (ghost ids) → Am (LRU). Promotion is earned by missing while in the ghost list, i.e. by evidence of reuse at a useful distance. |
| `ARC` | | | Adaptive Replacement Cache (FAST'03). T1/T2 plus ghost lists B1/B2 and one adaptive target `p`. A miss remembered in B1 says "we evicted from the recency half too early" and grows `p`; a miss in B2 says the same of the frequency half and shrinks it. The policy to beat. |
| `S3FIFO` | `s3-fifo` | `small-size-ratio=f` (0.10), `ghost-size-ratio=f` (0.90), `move-to-main-threshold=k` (1) | S3-FIFO (SOSP'23). Three FIFOs and a 2-bit counter; no pointer surgery on the hot path. Built on the observation that most objects in a real workload are requested exactly once. |
| `LRU-K` | `lru2` | `k=K` (2, max 8) | LRU-K (SIGMOD'93). Evicts by the *K*-th most recent reference, so one touch is not enough to look hot. `k=1` is exactly LRU. |
| `W-TinyLFU` | `tinylfu` | `window-ratio=f` (0.01), `protected-ratio=f` (0.80) | A small admission window, a segmented main region, and a count-min sketch deciding admission. The expensive question is not what should leave but whether something should come in at all. |
| `Belady` | `opt`, `min` | | Offline optimal: always evict the object whose next request is furthest away. **Requires an oracle trace** (every request carrying `next_access_vtime`); running it without one is a `ConfigError`, not a quietly meaningless number. The only honest upper bound on what any online policy could have achieved. |

### Adding your own

A policy is one header. No core file changes.

```cpp
#include "internal/datastructures/queue.hpp"
#include "internal/eviction/eviction_policy.hpp"

class MyPolicy final : public cachesim::IEvictionPolicy {
 public:
  explicit MyPolicy(const cachesim::PolicyConfig& config)
      : queue_(config.entry_hint),
        threshold_(config.params.getInt("threshold", 3)) {}

  std::string name() const override { return "MyPolicy"; }

  cachesim::Status onAdmit(internal::CacheEntry* e, const Request&) override {
    return queue_.pushFront(e);
  }
  cachesim::Status onHit(internal::CacheEntry* e, const Request&) override {
    return queue_.moveToFront(e);
  }
  internal::CacheEntry* evict() override { return queue_.popBack(); }
  cachesim::Status onRemove(internal::CacheEntry* e) override { return queue_.unlink(e); }

 private:
  cachesim::Queue<> queue_;
  std::int64_t threshold_;
};

// Register it; a later registration of an existing name replaces it, so you
// can override a built-in with your own version.
cachesim::PolicyRegistry::instance().add(
    {"MyPolicy", {"mine"}, "what it does", [](const cachesim::PolicyConfig& c) {
       return std::make_unique<MyPolicy>(c);
     }});
```

See [`include/policies/README.md`](include/policies/README.md) for the full
interface contract, including the hooks a non-trivial policy needs (`onMiss`,
`onResize`, `clear`) and what each one guarantees.

---

## Architecture

```
                   ┌──────────────────────────────────────┐
   trace file ───▶ │ ITraceReader      nextBatch(...)     │
                   │  ├─ byte source: mmap / file / zstd  │
                   │  └─ record layout: fields → Request  │
                   └──────────────────┬───────────────────┘
                                      │  Request (32 B)
                                      ▼
   ┌──────────────────────────────────────────────────────────────┐
   │ Cache::access(req)                                           │
   │                                                              │
   │   CacheStructure              IEvictionPolicy                │
   │   ───────────────              ───────────────               │
   │   obj_id → CacheEntry*         onHit / onMiss / onAdmit      │
   │   block arena of entries       evict / onRemove / onResize   │
   │   byte + object accounting                                   │
   │                                     │                        │
   │                                     ▼                        │
   │                      Queue · PriorityQueue · RandomBag       │
   │                      IdHistory · FrequencySketch             │
   │                                                              │
   │   Stats          ICacheObserver (eviction notifications)     │
   └──────────────────────────────────────────────────────────────┘
```

The load-bearing idea is the **metadata pointer**. A `CacheEntry` holds
identity, size, and one pointer to a node owned by whichever data structure
currently orders it:

```
CacheStructure::index        obj_id ──▶ CacheEntry
CacheEntry::metadata                   ──▶ QueueEntry / heap node / bag node
that node's cache_entry                ◀──
```

Both directions are O(1) with no hash lookup in either, which is what makes a
hit two pointer writes rather than a map probe. An entry is only ever in one
such structure at a time, so there is no conflict over the single pointer —
and a policy that needs several (ARC's T1 and T2, S3FIFO's three queues) just
moves the entry between them.

`CacheStructure` owns identity and size and makes them private, writable only
by itself. A policy can therefore be arbitrarily wrong about *ordering*
without being able to corrupt the cache's accounting. That separation is why
each policy can be checked against an independent reference and trusted
afterwards.

---

## Design decisions, and why

Each of these was a real fork in the road. The reasoning is recorded because
the alternative is re-litigating them later from the code alone.

**Virtual dispatch for policies, not templates.** `Cache<LRUPolicy>` would
inline the policy into the hot loop and is strictly faster per call. It was
rejected because a config file naming `s3fifo` cannot select a template
argument, and a sweep holding fifteen different policies at once needs them to
share a type — so a type-erasure layer comes back anyway. What is left is one
indirect call per request to a target that never changes for the life of a
`Cache` instance, which any modern branch predictor learns immediately. The
allocation-per-request and pointer-chasing problems were always the larger
cost; those are fixed structurally.

**A block arena, not one reserved vector.** Entry addresses are handed to
policies and stored inside metadata nodes, so they must never move — which
rules out a vector that can reallocate. Reserving one and refusing to grow
past it (the earlier design) makes the reservation a hard cap determined by
the *smallest* object in the trace, which a caller usually cannot know in
advance, and getting it wrong aborts a sweep hours in. The arena is a list of
blocks: the first is sized from `entry_hint`, so the common case is one
contiguous block with exactly a vector's locality, and if the trace turns out
to hold more small objects, further blocks are appended. Growth is geometric,
so a badly wrong hint costs O(log n) allocations in total.

**One typed metadata pointer, not a union.** libCacheSim's `cache_obj_t`
carries a union of ~20 per-policy structs; every object pays for the largest
and adding a policy edits a shared header. Here per-object policy state lives
in the node of the structure that owns the entry (`Queue<bool>` for Clock's
reference bit, `Queue<std::uint8_t>` for S3FIFO's counter), so it costs
nothing when unused and nothing shared changes when a policy is added.
The pointer is typed (`MetadataNode*`) rather than `void*` so that "is this
entry mine?" is answerable without guessing a field offset — which was
undefined behaviour that happened to work, right up until a policy held two
different kinds of structure at once.

**`Request` is 32 bytes and carries three things beyond `(id, size, time)`.**
`next_access_vtime`, `cpu_id` and `op`. The alternative — a side-channel array
keyed by request index — costs a second random memory stream in the hot loop
and has to be threaded through every reader, policy and plugin. The line that
keeps this from becoming a kitchen sink: a field earns its place only if a
trace reader can produce it *and* more than one consumer needs it. TTL,
tenant, cost and ML feature vectors do not pass, and are not here.

**Policies get their configuration at construction, not at attach time.** Most
of them need capacity before they can build anything — ARC sizes its ghost
lists from it, S3FIFO splits it 10/90, every queue reserves its node pool from
it. `onAttach(CacheStructure&)` stays for the one thing it is actually for:
reaching the structure itself.

**`evict()` selects *and* detaches.** The earlier `selectVictim()` + `onEvict()`
pair required victim selection to be side-effect-free, which Clock, Sieve and
S3FIFO cannot honour — all three mutate state while scanning (advancing a
hand, clearing bits, promoting between queues). The split forced them either
to lie about constness or to scan twice.

**`onMiss` fires before eviction.** ARC, 2Q and S3FIFO decide how to adapt
from whether the missed id was in a ghost list, and that decision changes
which list the following eviction should take from. Ordering it the other way
round silently breaks all three.

**No synchronization anywhere in the engine.** One `Cache` is one
configuration on one thread. A sweep is N independent objects; the only thing
they share is a read-only trace. libCacheSim's barrier queue exists because
its reader is shared mutable state, and that is the thing being avoided.

---

## Trace formats

Format names are libCacheSim's, so an existing `trace.type` carries over
unchanged. Compression is detected by magic number first and extension second
— the extension says what someone named the file, the magic says what it is.

| Name | Aliases | Record | Oracle | State |
|---|---|---|---|---|
| `oracleGeneral` | `oracleGeneralBin`, `oracle` | 24 B: `u32` time, `u64` id, `u32` size, `i64` next-access | yes | layout done |
| `lcs` | | 8 KiB header + versioned records, v1–v8 | yes | layout done |
| `binary` | `bin` | user-described via `format=<IQIq` + column indices | optional | layout done |
| `vscsi` | | v1 (32 B) and v2 (40 B), version auto-detected; SCSI command codes mapped to read/write; microsecond timestamps normalized to seconds | no | layout done |
| `twrBin` | `twr`, `twitter` | 20 B, with key/value sizes and op/TTL bit-packed | no | layout done |
| `mergedTrace` | `merged` | 9 B: `u64` vaddr, `u8` cpu; zstd batches | no | reader pending |
| `csv` | | delimited text, column mapping in reader params | optional | reader pending |
| `txt` | `plain`, `text` | one object id per line | no | reader pending |

Reader parameters use libCacheSim's key names, normalized so `obj-id-col`,
`obj_id_col` and `ObjIdCol` are one key: `obj-id-col`, `time-col`,
`obj-size-col`, `next-access-vtime-col`, `op-col`, `cpu-col`, `delimiter`,
`has-header`, `format`, `obj-id-is-num`, `page-shift`, `block-size`.

One deliberate deviation: libCacheSim reads every binary field as *signed*,
which sign-extends an unsigned 32-bit field whose top bit is set. For an
object id either reading is injective so hit ratios are unaffected, but for a
size field the signed reading yields a negative number. Fields are read as
declared here.

Details and the layout table are in
[`include/internal/trace/README.md`](include/internal/trace/README.md).

---

## Testing

```bash
ctest --test-dir build --output-on-failure
```

Three suites, about 181,000 assertions, under a second. No external test
framework: the simulator's only third-party dependency is one header-only hash
map, and a framework fetched at configure time would make the build need the
network. `tests/test_support.hpp` is forty lines.

The core technique is **reference cross-validation**. For every policy whose
definition is short enough to re-implement obviously-correctly, the test file
contains a deliberately naive model — `std::list`, O(n) scans, no cleverness —
and the engine's hit count must match it request for request across twelve
trace and capacity combinations. Being absurdly slow is the point: the
reference can be read and checked against the paper by eye. ARC's is a direct
transcription of the FAST'03 pseudocode.

Matched exactly: LRU, FIFO, MRU, LFU, LFU-DA, Clock, Sieve, ARC, 2Q, LRU-1/2/3,
SLRU-4, Belady.

This found five bugs, none of which would have crashed anything. Each would
have produced a *slightly wrong hit ratio* — which is the failure mode that
survives all the way into a published number:

1. `IdHistory`, the ghost-list structure, bounded *slots* rather than *live
   ids*. Removal by id is its dominant operation, so it filled with
   tombstones, reached its bound while only a fraction of its ids were live,
   and began dropping live ghosts. ARC then adapted `p` on a directory that
   had quietly lost entries.
2. ARC omitted the one eviction in the algorithm that leaves no ghost (case IV
   with T1 filling the cache), changing B1's contents and every subsequent
   adaptation.
3. `LRU-K` with `k=1` was not LRU — an admission is already the first
   reference, and treating it as "unseen" made the policy evict by insertion
   order.
4. `W-TinyLFU` was *exactly* LRU and did not respond to any of its own
   parameters, for two independent reasons: the main region could never
   bootstrap (window overflow was moved in only after winning a contest, so
   the first candidate had nothing to be weighed against and was evicted
   immediately), and the count-min sketch was sized at four counters per entry
   rather than sixteen, so on any trace with many more objects than cache
   slots most estimates saturated and every comparison was a tie.
5. `LFU-DA` had an undefined tie-break. Every newly admitted object shares a
   key, so most comparisons on a churning cache were ties and the order was
   left to whatever the heap happened to do — making runs irreproducible.

Beyond the references there are two more layers: **universal invariants**, run
over every policy including the three with no tractable reference (occupancy
checked against capacity on *every* request, a capacity of 1 must not
deadlock, a cache larger than the working set must evict nothing); and **known
relationships** (Belady bounds every online policy, LRU-1 is LRU, SLRU-1 is
LRU, the scan-resistant policies beat LRU on a hot-set-plus-scan trace).

More in [`tests/README.md`](tests/README.md).

---

## Repository layout

```
include/
  internal/            engine internals — see internal/README.md for the rule
    cache/             Cache, CacheStructure, CacheEntry, Stats
    datastructures/    Queue, PriorityQueue, RandomBag, IdHistory,
                       FrequencySketch, BlockPool, MetadataNode
    eviction/          IEvictionPolicy, PolicyRegistry
    request/           Request
    trace/             byte sources, record layouts, reader interface
    common/            hashing, string/size parsing, errors, compiler shims
  policies/            one header per eviction policy
src/                   out-of-line definitions, mirroring include/
tests/                 three suites plus a 40-line harness
examples/              replay.cpp — a complete, runnable usage example
third_party/ankerl/    unordered_dense (MIT), the only external dependency
```

Every directory has its own `README.md` explaining what is in it and why.

`internal/` is a *suggestion*, not a wall: it holds the machinery of the cache
itself, which you should not need to modify to add a policy or read a new trace
format. Nothing stops you if you decide you need to.

---

## Roadmap

In order:

1. **Trace readers.** `openTrace()` over the layouts already in place, plus
   CSV, text and mergedTrace; object-level sampling and request limits.
2. **YAML config.** libCacheSim's schema (`trace`, `global`, `output`,
   `configurations`) so existing config files run unchanged.
3. **Sweep runner.** A thread pool over configurations, with the trace either
   materialized once into a shared read-only array (so no worker parses
   anything) or streamed per worker from a shared mapping, chosen by a memory
   budget.
4. **CLI.** `cachesim config.yaml`, plus a direct-argument mode.
5. **Benchmarks, and a measured comparison against libCacheSim** on the same
   traces and the same machine. Until that exists, this README makes no
   performance claim relative to libCacheSim, only the structural argument at
   the top.
6. **Plugin layer.** Request filters and periodic hooks, for modelling things
   outside the cache that have to stay coherent with it (a per-CPU TLB
   absorbing hits, a PEBS-style sampler dropping requests). `ICacheObserver`
   is the first piece and already works.

Not planned, per the project's scope: MRC profiler, trace analyzer, admission
policies. A prefetcher hook is deliberately left room for.

---

## License and credits

`third_party/ankerl/unordered_dense.h` is Martin Leitner-Ankerl's
[unordered_dense](https://github.com/martinus/unordered_dense), MIT licensed.

The policies implement published algorithms; the papers are cited in each
policy's header. [libCacheSim](https://github.com/1a1a11a/libCacheSim) is the
reference this project measures itself against, and its trace formats are what
this one reads.
