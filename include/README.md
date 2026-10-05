# `include/`

Two directories, split by who is expected to touch them.

## `internal/` — the machinery of the cache

The request type, the object arena, the hash index, the byte accounting, the
eviction sequencing, the trace plumbing. You should not need to modify any of
it to add an eviction policy or to read a new trace format, which is the whole
point of the name.

It is a *suggestion*, not a wall. There is no access control, nothing is
hidden, and if you decide you need to change something in here, change it.
"Internal" means "this is the cache's own business, and if you are editing it
you are probably solving a different problem than you think you are" — not
"keep out".

## `policies/` — one header per eviction policy

This is where new work normally goes. A policy is a single header that
implements `IEvictionPolicy` and composes the data structures from
`internal/datastructures/`. Adding one touches no other file; registering it
is one call.

---

### Include paths

Both directories are on the include path as given, so headers are included by
their path under `include/`:

```cpp
#include "internal/cache/cache.hpp"
#include "internal/datastructures/queue.hpp"
#include "policies/arc.hpp"
```

### Namespaces

| Namespace | Holds |
|---|---|
| `cachesim` | Everything a user touches: `Cache`, `Request`, `Stats`, `IEvictionPolicy`, `PolicyRegistry`, `Queue`, `PriorityQueue`, `IdHistory`, `RandomBag`, `FrequencySketch`, the error types, the parsing helpers. |
| `cachesim::internal` | Types a policy handles but never constructs: `CacheEntry`, `CacheStructure`, `MetadataNode`, `BlockPool`, the trace byte sources and record layouts. |
| `cachesim::policies` | The concrete policies. |
| `cachesim::test` | The test harness only. |

The data structures are in `cachesim` rather than `cachesim::internal` on
purpose: a policy author uses them constantly, and making them look internal
would be misleading about who they are for.

### Header-only versus compiled

Most of this is header-only, for two different reasons.

*Templates* have to be — `Queue<Payload, Tag>`, `PriorityQueue<Priority,
Payload, Compare>`, `BlockPool<T>` and `RandomBag<Payload>` must be visible
wherever they are instantiated.

*The hot path* benefits from it. `Cache::access` and `CacheStructure::find`
are defined inline so that the compiler can see the whole hit path at the
replay loop's call site and keep the index pointer and the stat counters in
registers across requests. The cold paths — the miss branch, eviction, every
error — are out of line in `src/`, which keeps the inlined footprint small
enough that inlining it is actually a win.

What is compiled: `Cache`'s cold paths, the policy registry, trace byte
sources, record layouts, format detection. See [`../src/README.md`](../src/README.md).
