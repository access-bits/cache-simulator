# `tests/`

```bash
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Three suites, about 181,000 assertions, under a second.

| Suite | Covers | Assertions |
|---|---|---|
| `datastructures_test` | `CacheStructure`, `Queue`, `PriorityQueue`, `IdHistory`, `RandomBag`, `FrequencySketch` | ~40,500 |
| `cache_test` | the `Cache` engine: hit/miss, byte accounting, eviction sequencing, size changes, oversized objects, observers, error reporting | ~20,100 |
| `policy_test` | all 15 policies | ~120,400 |

Run one directly to see the case names:

```bash
./build/tests/policy_test
```

---

## No test framework, on purpose

`test_support.hpp` is forty lines: `CHECK`, `CHECK_EQ`, `CHECK_NEAR`,
`TEST_CASE`, a failure count and a summary.

The simulator's only third-party dependency is one header-only hash map.
Adding GoogleTest or Catch2 would make the build fetch something at configure
time, which means the build needs the network — on a cluster node, in a
container, or three years from now when the URL has moved, that is the
difference between "builds" and "does not build". The thing being bought for
that price is assertions, named cases, and a non-zero exit code.

---

## The three layers

### 1. Reference cross-validation

For every policy whose definition is short enough to re-implement
obviously-correctly, the test file contains a second implementation — written
as naively as possible, with `std::list` and O(n) scans — and the engine's hit
count must match it **request for request** across twelve trace and capacity
combinations (3 seeds × 4 capacities, 3,000 requests over 300 objects, Zipf-ish).

Being absurdly slow is the point. `refArc` is a direct transcription of the
FAST'03 pseudocode in object counts, so it can be read next to the paper and
checked by eye; it would be useless as production code and that is fine. The
byte-based implementation must reduce to it exactly when every object is size 1.

Matched exactly: LRU, FIFO, MRU, LFU, LFU-DA, Clock, Sieve, ARC, 2Q,
LRU-1/2/3, SLRU-4, Belady.

Not cross-validated, because no tractable reference exists: S3FIFO, W-TinyLFU,
Random. These are covered by the invariants below and by the scan-resistance
relationships.

### 2. Universal invariants

Run over **every** registered policy, so a new policy opts in by existing:

- Occupancy never exceeds capacity — checked on *every single request*, not at
  the end. An intermediate overflow that later resolves is still a bug, and
  only a per-request check catches it.
- Capacity 1 does not deadlock. This is where every eviction path runs on an
  almost-empty structure, and where an off-by-one in a budget split means an
  infinite loop rather than a wrong number.
- A cache larger than the working set evicts nothing and hits exactly
  `requests − distinct objects` times.
- Variable object sizes respect a byte capacity.
- `clear()` followed by a refill works, and nothing survives it.

### 3. Known relationships

- **Belady bounds every online policy.** A policy that beats the offline
  optimum has a bug, and this is the cheapest possible way to find out.
- `LRU-K` with `k=1` is exactly LRU. `SLRU` with one segment is exactly LRU.
- `Clock` beats `FIFO` on a frequency-skewed trace — a Clock that failed to
  use its reference bit would score FIFO's number exactly.
- The scan-resistant policies beat LRU on a hot-set-plus-scan trace: 50 hot
  objects passed over four times, then a scan of objects never seen again, in
  a cache of 100.
- **2Q's limit, pinned down deliberately.** With a *longer* scan, 2Q drops to
  exactly LRU's hit count, because its resistance comes entirely from A1out
  and a scan longer than A1out ages the working set's ids out of it. That is a
  property of the algorithm rather than a defect, and it is the difference
  between 2Q and the policies whose evidence lives on the cached objects
  themselves (ARC's T2, S3FIFO's main queue, W-TinyLFU's sketch), which keep
  working. Asserting the equality means a future change that accidentally
  "fixes" it gets noticed.

---

## The five bugs this found

None would have crashed anything. Each would have produced a *slightly wrong
hit ratio* — the failure mode that survives all the way into a published
number.

**1. `IdHistory` dropped live ghost entries.**
The original implementation was a ring of ids plus an `id → slot` map, with a
tombstone left on removal so every operation stayed O(1). But that bounds
*slots*, not *live ids* — and removal by id is a ghost list's dominant
operation, since almost every ghost leaves by being promoted back into the
cache rather than by ageing out. The ring filled with tombstones, reached its
bound while only a fraction of its ids were live, and began evicting live
ghosts. ARC then adapted `p` on a directory that had quietly lost entries.
Caught by the ARC reference, and only at capacity 64 and above, where the
ghost lists see enough churn for it to matter.

**2. ARC omitted the one eviction that leaves no ghost.**
In case IV with T1 filling the whole cache, B1 is empty and the |T1|+|B1| ≤ c
bound can only be relieved by shrinking T1 — so the object that leaves is
*discarded*, not recorded in B1. Ghosting it would push the sum straight back
to the bound it was just brought under. Getting this wrong changes B1's
contents and with them every later adaptation of `p`.

**3. `LRU-K` with `k=1` was not LRU.**
An admission *is* the first reference, so for K=1 the object's 1st-most-recent
reference time is now, not "unseen". Treating it as unseen made LRU-1 evict by
insertion order. The cross-validation at `k=2` and `k=3` passed throughout —
the bug lived only in the boundary case, which is exactly why the
`LRU-1 == LRU` relationship is asserted separately.

**4. `W-TinyLFU` was exactly LRU, and ignored all its own parameters.**
Two independent causes, either of which alone was enough:

- The window's overflow was moved into probation only *after* winning the
  admission contest. So the first time the cache filled, the candidate being
  considered was the main region's only occupant, had nothing to be weighed
  against, and was evicted again immediately. The main region stayed
  permanently empty and every eviction came off the window's tail.
- The count-min sketch was sized at four counters per cache entry instead of
  sixteen. On any trace whose object count far exceeds the cache size — which
  is every interesting trace — collisions saturated most estimates, every
  admission comparison became a tie, and the filter stopped filtering.

The tell was that the hit ratio was *identical* for every `window-ratio` from
0.01 to 0.5. A policy that ignores its own parameters is not working, whatever
number it is producing.

**5. `LFU-DA` had an undefined tie-break.**
Every newly admitted object has key `age + 1`, so on a cold or churning cache
most comparisons are ties, and the order was left to whatever the heap
happened to do — making runs irreproducible. Ties now break FIFO on "reached
this key first", matching exact LFU.

---

## Adding tests

A case is a scope:

```cpp
TEST_CASE("Queue: popBack detaches the entry") {
  Arena arena;
  Queue<> queue(8);
  internal::CacheEntry* a = arena.add(1);
  queue.pushBack(a);
  CHECK_EQ(queue.popBack(), a);
  CHECK(a->metadata == nullptr);
}
```

`CHECK_EQ` prints both values on failure and stringifies anything streamable,
including pointers. `CHECK_NEAR` takes a tolerance. Only failures print, so a
passing run stays readable and a failing one points straight at the line.

Traces come from helpers in `policy_test.cpp`: `makeTrace` (Zipf-ish, with the
oracle column filled in by a backward pass) and `makeScanTrace` (a hot set
looped over, with a scan between loops). Both take an explicit seed — every
test is deterministic, and a failure reproduces exactly.
