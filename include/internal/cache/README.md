# `include/internal/cache/`

The engine: what is cached, where to find it, and the sequence of steps one
request goes through.

| Header | Role |
|---|---|
| `cache_entry.hpp` | `CacheEntry` — 24 bytes: identity, size, one metadata pointer. |
| `cache_structure.hpp` | `CacheStructure` — the object arena and the `obj_id → CacheEntry*` index. Knows nothing about ordering. |
| `cache.hpp` | `Cache` — the request lifecycle, byte accounting, eviction sequencing, statistics. |
| `stats.hpp` | `Stats` — plain counters, ratios derived on demand. |

---

## `CacheEntry`

```cpp
class CacheEntry {
 public:
  MetadataNode* metadata = nullptr;     // public: whoever orders this entry owns it
  std::uint64_t objId() const;
  std::uint32_t size() const;
 private:
  friend class CacheStructure;          // only it may write these
  std::uint64_t obj_id_;
  std::uint32_t size_;
};
```

24 bytes, so three entries fill two cache lines and a scan over the arena —
which Clock, Sieve and Random all do in one form or another — streams.

The asymmetry is deliberate. `metadata` is public because handing it out *is*
the mechanism: a `Queue` writes its node's address there, and the node points
back, giving O(1) in both directions with no hash lookup. Identity and size
are private because the index and the byte accounting are keyed on them.

## `CacheStructure`

```cpp
CacheEntry* find(std::uint64_t obj_id) const;   // nullptr if not cached
CacheEntry* admit(std::uint64_t obj_id, std::uint32_t size);
Status      erase(CacheEntry* entry);
Status      resize(CacheEntry* entry, std::uint32_t new_size);
void        clear();
std::uint64_t occupiedBytes() const;
std::size_t   objectCount() const;
```

Two invariants drive the design:

1. **Entry addresses never move.** They are handed to policies and stored
   inside metadata nodes, so a reallocation that moved them would silently
   corrupt every policy at once.
2. **No allocation per request.** libCacheSim calls `malloc` once per object
   insert and `free` once per eviction; on a trace with a 90% miss ratio that
   is an allocator round trip for nearly every request.

### Why a block arena

A single `std::vector` reserved once satisfies both — but only by making its
reserved size a hard cap, because growing it would move every entry. That cap
is a genuine hazard: it depends on the *smallest* object in the trace, which a
caller usually cannot know in advance, and getting it wrong aborts a sweep
hours in. (The earlier version of this file reserved `1.5 × max_entries` and
returned `nullptr` past it, which the `Cache` turned into a fatal exception.)

So the arena is a list of blocks:

```
entry_hint = 4096                   blocks:  [ 4096 entries ]   ← one allocation,
                                                                  one contiguous run

more small objects than expected:   blocks:  [ 4096 ][ 2048 ][ 3072 ] ...
                                             ^ existing blocks are never touched,
                                               so every outstanding pointer stays valid
```

The first block is sized from `entry_hint`, so the overwhelmingly common case
— uniform object sizes, or an object-count capacity — is a single contiguous
block with exactly the locality the vector had. Growth is geometric and capped
at 1M entries per block, so a badly wrong hint costs O(log n) allocations in
total rather than one per object.

`max_entries` survives as a genuine opt-in hard cap (default `0`, unlimited),
for a caller who really does want to bound memory and treat exhaustion as an
error.

### `erase` refuses an entry that is still tracked

```cpp
Status erase(CacheEntry* entry) {
  if (entry == nullptr || entry->metadata != nullptr) return Status::kFailure;
  ...
}
```

A policy that forgets to unlink before the slot is recycled leaves a dangling
node pointing at a slot that will be handed to a different object. That is a
use-after-free that would show up as a wrong hit ratio, so it is checked
rather than trusted.

### `admit` does not re-check for the key

Every caller has just performed the `find()` that told it this was a miss.
Checking again would double the cost of the hot path for no information. The
contract is in the header; the one caller honours it.

## `Cache`

```cpp
bool access(const Request& req);        // true on hit
bool remove(std::uint64_t obj_id);      // explicit invalidation
void clear();                           // empty the cache, keep the counters
void resetStats();                      // keep the cache, zero the counters
void addObserver(ICacheObserver*);
```

### The request lifecycle

```
access(req)
  │
  ├─ hit:   stats++ ──▶ policy.onHit(entry, req)
  │                      └─ if the request named a new size:
  │                           structure.resize ─▶ policy.onResize ─▶ evict until it fits
  │
  └─ miss:  policy.onMiss(req)                 ← before eviction, deliberately
            │
            ├─ object larger than the whole cache? count n_oversized, stop
            │
            ├─ while occupied + size > capacity:
            │     victim = policy.evict()      ← selects AND detaches
            │     structure.erase(victim)
            │     observers.onEvicted(...)
            │
            └─ entry = structure.admit(...) ──▶ policy.onAdmit(entry, req)
```

`onMiss` runs before eviction because ARC, 2Q and S3FIFO decide how to adapt
from whether the missed id was in a ghost list, and that decision changes
which list the following eviction should take from. Ordering it the other way
round silently breaks all three.

### Why `access` is inline and everything else is not

`access` is defined in the header; `missPath`, `evictToFit`, `evictOne`,
`onHitSizeChanged` and every error path are `CACHESIM_NOINLINE` in `src/`.

With the hit path visible at the replay loop's call site, the compiler keeps
the index pointer and the stat counters in registers across requests and folds
a hit down to a hash probe plus one call into the policy. Keeping the cold
paths out of line is what makes that worth doing — inlined, they would blow
the loop's instruction footprint for code that runs on a minority of requests.

### Size changes on a hit

Traces do re-write an object at a new size, and handling it is more delicate
than it looks, because growing an object can require evicting — possibly
including the object just hit. The order is therefore:

1. `policy.onHit(entry, req)` while the entry is *certainly* still alive.
2. `structure.resize` and `policy.onResize`, so the policy can fix its own
   byte totals (a `Queue` cannot know its contents changed size).
3. Evict until it fits.

An object that grows past the entire capacity is removed outright rather than
left to the eviction loop, which keeps "the cache is never over capacity" true
at every exit. The request is still reported as a hit — it genuinely was one —
and `update_size_on_hit = false` turns the whole behaviour off for studies
that prefer occupancy monotone in the trace's unique-byte footprint.

### Diagnosable failures

Every `EngineError` names the policy and the state:

```
engine error: IEvictionPolicy::evict returned no victim for policy S3FIFO
while the cache was over capacity (1048576 of 1048576 bytes, 237 objects)
— the policy is not tracking every admitted object
```

`"onHit failed"` is not something you can act on eight hours into a sweep.

## `Stats`

Plain counters; every ratio is a method. That is what lets a warm-up phase be
discarded by zeroing the struct and two partial runs be summed without a
derived quantity going stale.

Two counters that are not just bookkeeping:

- **`n_oversized`** — requests for objects larger than the whole cache. They
  are counted as misses, because they are, but reported separately: a run with
  a large count here is telling you the capacity is below the object size
  distribution, which otherwise looks like a mysteriously bad policy.
- **`n_filtered`** — requests dropped before the cache saw them, by a future
  filter plugin. Deliberately *not* in `n_req`: the cache genuinely never saw
  them, and folding them in would make the miss ratio mean something else.

`obj_metadata_size` is charged against occupancy but not against
`n_req_byte`/`n_hit_byte`, so a byte miss ratio still refers to real traffic
rather than to traffic plus the simulator's accounting of its own overhead.
