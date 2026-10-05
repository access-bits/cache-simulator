# `include/internal/`

The cache's own machinery, in five layers. Each depends only on the ones above
it, which is what keeps a change to one from rippling through the rest.

```
common/           hashing, parsing, errors, compiler shims
   ▲
request/          Request — one trace event
   ▲
cache/            CacheEntry, CacheStructure, Cache, Stats
   ▲                      ▲
datastructures/   ────────┘   Queue, PriorityQueue, RandomBag,
   ▲                          IdHistory, FrequencySketch, BlockPool
eviction/         IEvictionPolicy, PolicyRegistry
   ▲
                  ../policies/*  (one header each)

trace/            byte sources, record layouts, reader interface
                  — depends only on request/ and common/
```

`trace/` sits off to the side deliberately: it produces `Request` objects and
knows nothing about caches, so a trace reader can be tested, replaced or
reused without touching the engine at all.

---

## What is in each directory

### [`common/`](common/)

| Header | Contents |
|---|---|
| `compiler.hpp` | `CACHESIM_ALWAYS_INLINE`, `CACHESIM_LIKELY`, `CACHESIM_NOINLINE`, `kCacheLineSize`. Every one degrades to a no-op on a compiler that lacks it, so no translation unit has to care which compiler it is being built with. |
| `hash.hpp` | `mix64` (splitmix64's finalizer), `hashBytes` (FNV-1a, off the hot path only), and `Rng` (xoshiro256++). |
| `string_util.hpp` | `parseSize` ("100MB", "1GiB", "512k"), `parseBool`, `normalizeKey`, and `ParamMap` — the `key=value` bag that carries every algorithm- and reader-specific option. |
| `error.hpp` | `Error` and its three subclasses. |

**Why there is a hand-written RNG.** Not to be clever: `std::mt19937_64` is
fine, but the C++ standard does not specify the output of `std::uniform_int_distribution`,
so the same seed gives different draws on different standard libraries. A
simulation has to be bit-for-bit reproducible from its seed for a result to be
checkable, so the generator *and* the uniform draw are both ours.
`Rng::below` uses Lemire's multiply-shift: one multiply in the common case, no
division, unbiased after a rarely-taken rejection branch.

**Why `mix64` is used everywhere an id is hashed.** Trace object ids are almost
never uniformly distributed — block traces hand out sector-aligned LBAs,
memory traces hand out page numbers with the low bits already shifted off,
key-value traces hand out dense counters. Feeding those straight into a modulo
or a bucket index clusters badly; mixing first does not.

**Why `ParamMap` tracks which keys were read.** `unusedKeys()` reports every
key nobody asked about, so a misspelled `sampling_perod=1000` can be rejected
loudly instead of silently running with a default. A typo that quietly does
nothing is the kind of thing that invalidates a week of sweep results.

### [`request/`](request/)

`Request` — one trace event, 32 bytes, trivially copyable, two per cache line.
The header records the rule that keeps it from growing into libCacheSim's
kitchen-sink `request_t`: a field earns its place only if a trace reader can
produce it *and* more than one consumer needs it.

### [`cache/`](cache/)

`CacheEntry`, `CacheStructure`, `Cache`, `Stats`. See
[`cache/README.md`](cache/README.md).

### [`datastructures/`](datastructures/)

The intrusive structures the policies are built from, and the metadata-pointer
design that makes them interchangeable. See
[`datastructures/README.md`](datastructures/README.md).

### [`eviction/`](eviction/)

`IEvictionPolicy` — the interface, with the call order for one request and
what each hook guarantees — and `PolicyRegistry`, the single point where a
runtime string becomes a concrete policy.

### [`trace/`](trace/)

Byte sources, binary record layouts, format detection, and the reader
interface. See [`trace/README.md`](trace/README.md).

---

## The one invariant worth stating loudly

**A `CacheEntry`'s identity and size are private to `CacheStructure`.**

A policy receives `CacheEntry*` and can read `objId()` and `size()`, and can
hand the entry to a data structure that writes its `metadata` pointer. It
cannot change the id or the size, because those are what the hash index and
the byte accounting are keyed on, and a policy changing either behind
`CacheStructure`'s back would desynchronize both.

The consequence is worth the restriction: a policy can be arbitrarily wrong
about *ordering* without being able to corrupt the cache's accounting. That is
why each policy can be checked against an independent reference implementation
and then trusted, and why a bug in a policy shows up as a wrong hit ratio in
one row of a sweep rather than as memory corruption in all of them.
