# `include/internal/common/`

Small shared utilities. Nothing here knows anything about caches.

| Header | Contents |
|---|---|
| `compiler.hpp` | `CACHESIM_ALWAYS_INLINE`, `CACHESIM_NOINLINE`, `CACHESIM_HOT`, `CACHESIM_LIKELY`/`UNLIKELY`, `CACHESIM_PREFETCH`, `kCacheLineSize` |
| `hash.hpp` | `mix64`, `hashBytes`, `Rng` |
| `string_util.hpp` | `trim`, `toLower`, `iequals`, `normalizeKey`, `split`, `parseInt`, `parseDouble`, `parseBool`, `parseSize`, `ParamMap` |
| `error.hpp` | `Error`, `ConfigError`, `TraceError`, `EngineError` |

---

## `compiler.hpp`

Every macro degrades to a no-op on a compiler that lacks the underlying
attribute, so no translation unit has to guess which compiler it is being
built with. `kCacheLineSize` is there to pad hot per-thread state: false
sharing would serialize parameter-sweep workers that have no logical
interaction at all.

## `hash.hpp`

**`mix64`** is splitmix64's finalizer — a bijection on 64 bits with good
avalanche, meaning every input bit influences roughly half the output bits.
It is applied wherever a structured integer has to behave like a random one.

That is not decoration. Trace object ids are almost never uniformly
distributed: block traces hand out sector-aligned LBAs, memory traces hand out
page numbers with the low bits already shifted off, key-value traces hand out
dense counters. Feeding those straight into a modulo or a bucket index
clusters badly; mixing first does not.

**`hashBytes`** is FNV-1a, used only off the hot path — turning a non-numeric
object id from a CSV or text trace into the `uint64` id space the engine works
in. A collision would merge two distinct trace objects, which at 64 bits is
vanishingly unlikely for any real trace: the birthday bound puts it at roughly
one in a billion even at 190 million distinct keys.

**`Rng`** is xoshiro256++, seeded through splitmix64 (seeding xoshiro's 256
bits of state with mostly zeros makes its first outputs poor).

There is a hand-written generator here rather than `std::mt19937_64` for one
specific reason: the C++ standard does not specify the output of
`std::uniform_int_distribution`, so the same seed produces different draws on
different standard library versions. A simulation has to be bit-for-bit
reproducible from its seed for a result to be checkable by anyone else, so the
generator *and* the uniform draw both have to be ours.

`below(bound)` uses Lemire's multiply-shift: one multiply in the common case,
no division, and unbiased after a rejection branch that is almost never taken.
Random eviction calls it once per eviction, in the hot loop.

## `string_util.hpp`

**`normalizeKey`** lowercases and drops `_`, `-` and spaces. It is what makes
`S3FIFO`, `s3fifo` and `s3-fifo` one policy name, and `obj-id-col`,
`obj_id_col` and `ObjIdCol` one reader parameter. libCacheSim's own YAML files
mix all of these spellings, sometimes within one file.

**`parseSize`** accepts a plain byte count or a unit suffix, with both SI-style
(`100MB`, `100m`) and IEC (`100MiB`) spellings meaning powers of 1024 — which
matches libCacheSim, whose documentation lists `100MB` and `100MiB` as the
same thing, so existing configs carry over unchanged.

**`ParamMap`** is the `key=value` bag that carries every algorithm- and
reader-specific option. Separators are `,` or `;` between pairs and `=` or `:`
within one; a bare key reads as a `true` flag, so `...,verbose,...` works.

Its one unusual feature is that reading a key records it, so `unusedKeys()`
reports every key nobody asked about. That lets a caller reject a misspelled
`sampling_perod=1000` loudly instead of silently running with a default. A
typo that quietly does nothing is the kind of thing that invalidates a week of
sweep results without leaving a trace.

## `error.hpp`

Everything derives from `cachesim::Error`, so a caller can catch one type and
still print something useful.

| Type | Means |
|---|---|
| `ConfigError` | A config file, CLI argument or parameter string the user can fix. |
| `TraceError` | A trace file that is missing, unreadable, truncated, or not in the format it was declared to be. |
| `EngineError` | An invariant inside the engine was violated — a bug in a policy or in the engine, not user error. |

Exceptions are thrown only at layer boundaries where there is no sensible way
to continue. Inside the engine, recoverable conditions return `Status` and the
caller decides; `Cache` is the thing that converts an internal failure into an
`EngineError`, because once a policy and the cache structure disagree there is
nothing safe to do but stop.
