# Delivering one trace to many simulations

A parameter sweep runs N independent caches over the same trace. The caches
share nothing — that is the whole design — so the trace is the only coupling
left, and past a handful of configurations how that coupling is arranged
matters more than anything inside the cache engine.

Four arrangements are implemented, selectable at runtime, and they produce
identical results. `tools/replay_bench --all` runs each in turn and checks
that the miss counts agree, because a difference would be a delivery bug —
the kind that otherwise shows up as two sweep rows that quietly are not
comparable.

## The four

### `shared-ring` (default)

One reader thread decodes the trace once into a large shared ring. Each
consumer walks the ring behind its own cursor at its own pace. The reader may
only overwrite slots the **slowest** consumer has passed, so the ring's
capacity is exactly the slack permitted between the fastest and slowest
configuration.

```
                   reader ──► writes here
                              │
  ring  ┌───────────────────────────────────────────────┐
        │█████████████████████░░░░░░░░░░░░░░░░░░░░░░░░░│
        └───┬──────────┬──────────┬────────────────────┘
       slowest      config B   config A
       config C     (cursor)   (cursor)
       (cursor)
        └──────────── capacity ───────────────┘
             the reader cannot pass this point
```

Positions are **absolute** counts of requests since the start of the trace,
never wrapped indices; wrapping happens only when a position is turned into a
slot (`& mask`). That removes the usual ring-buffer ambiguity between empty
and full, and makes every comparison a subtraction on monotone numbers.

Cursors are published **coarsely** — once per `publish_stride` requests
(default 1M), not per request. Four things follow:

* **Staleness is safe in one direction only, and it is the right one.** A
  consumer that has not published yet makes the reader see a *smaller*
  min-cursor than reality, so the reader is conservative. Likewise a consumer
  never reads past the reader's last *published* frontier. Both cost latency;
  neither can cost correctness.
* **Each published cursor gets its own cache line.** Without the padding, N
  cursors share lines and every publication invalidates the line in every
  core reading it — which is precisely the contention coarse publication
  exists to avoid, just moved somewhere less visible.
* **Publication is an eventcount, not a notify.** A publisher takes the mutex
  and signals the condition variable only when a waiter count says somebody is
  actually asleep; in the common case — publishing while every consumer is
  busy — a publication is one atomic store and nothing else. The handshake is
  Dekker's and needs sequential consistency on exactly four accesses: the
  publisher stores its position then loads the waiter count, the waiter
  increments the count then loads the position, and one of the two must see
  the other. Relax any of the four and a wakeup can be lost, which shows up
  as a hang under load and never in a test. This is what makes the stride a
  free parameter rather than a cost: without it, every publication is a futex
  syscall whether or not anyone is listening, which is the pressure that
  pushes strides to be large.
* **The ring must be comfortably larger than the stride.** Both sides run up
  to one stride ahead of what they have published, so a ring of one or two
  strides could look full to the reader and empty to a consumer at the same
  time, and both would wait. The implementation requires at least four
  strides and clamps the stride if asked for less; realistic settings are
  thousands of strides. Belt and braces, both sides also publish
  unconditionally before blocking.

### `batch-barrier`

What libCacheSim does today, reproduced so its cost can be measured rather
than argued about. The reader fills a batch, waits for **every** consumer to
finish the previous one, copies the batch into each consumer's private
buffer, and releases them all.

Two costs, both structural:

* **The barrier.** No consumer gets batch k+1 until every consumer has
  finished batch k, so each batch takes as long as the slowest configuration
  and the reader idles for the difference. Worse, fast configurations can
  never finish early and release a core.
* **The copies.** Every request is memcpy'd once per configuration. With 70
  configurations that is 70 copies of the trace, and libCacheSim's
  `request_t` is over 100 bytes.

### `private-readers`

Each consumer opens its own reader. No synchronization at all, which is
appealing — but the trace is decoded N times, and for a compressed trace that
is N decompressions. On an uncompressed memory-mapped trace it is competitive,
because decoding is nearly free and the page cache does the sharing for you.

### `materialized`

Decode the whole trace once into a shared read-only array, then let every
consumer walk it with no synchronization whatsoever. Strictly the least work
of any of these, and the right answer whenever the trace fits in memory — at
32 bytes a request, a billion requests is 32 GB.

Its one weakness is that the load is **serial**: nothing starts until
everything is decoded. On a compressed trace that is a long cold start.

The ring degenerates to exactly this when its capacity exceeds the trace
length, so the runner detects that case and switches, rather than running a
reader thread and cursor traffic for a buffer that can never wrap.
`--no-auto-materialize` turns the detection off.

## Measured

4-core cloud container, 15 GiB RAM, 8 configurations, LRU at eight cache
sizes. Identical miss counts across all four in every run.

**zstd-compressed trace, 20M requests** — the shape of a real `mergedTrace`:

| strategy | wall | aggregate | per config |
|---|---|---|---|
| **shared-ring** | **4.05s** | 39.5 M req/s | 4.93 M req/s |
| batch-barrier | 4.74s | 33.8 M req/s | 4.22 M req/s |
| private-readers | 5.54s | 28.9 M req/s | 3.61 M req/s |
| materialized | 7.28s | 22.0 M req/s | 2.75 M req/s |

`private-readers` pays for eight decompressions; `materialized` pays 3.85s of
serial decompression before any simulation starts.

**Uncompressed trace, scaling in configuration count, 10M requests each:**

| configs | shared-ring | batch-barrier | private-readers | ring vs barrier |
|---|---|---|---|---|
| 2 | 0.67s | 0.74s | 0.83s | 1.10× |
| 4 | 0.71s | 1.48s | 1.02s | 2.08× |
| 8 | 2.22s | 2.93s | 2.25s | 1.31× |
| 16 | 3.72s | 4.48s | 4.30s | 1.20× |
| 32 | 7.55s | 9.41s | 7.88s | 1.24× |

Two caveats on these numbers, both of which understate the ring's advantage:

* **Four cores.** With more configurations than cores every strategy is
  CPU-bound, which compresses the differences. The barrier's real penalty —
  fast configurations unable to finish and release a core — needs cores to
  be releasable.
* **One reader is close to saturated already.** At eight consumers the
  reader thread is busy 3.23 of 4.05 seconds. Past that it becomes the
  bottleneck and every consumer waits on it rather than on each other. The
  fix is parallel decode, and `mergedTrace` is the ideal case for it: its
  batches are independent zstd frames, so they decompress concurrently with
  no coordination.

## Choosing

| situation | use |
|---|---|
| trace fits in memory | `materialized` — least work, no synchronization |
| trace larger than memory, compressed, or expensive to parse | `shared-ring` |
| uncompressed, memory-mappable, few configurations | `private-readers` is fine; the page cache already shares |
| comparing against libCacheSim's behaviour | `batch-barrier` |

## Tuning the ring

| option | default | what it does |
|---|---|---|
| `--ring-mb N` | 1024 | the slack budget. Bigger means a slow configuration can fall further behind before it throttles the reader. `stalls r=` in the output is how often that happened — if it is zero, the ring is big enough. |
| `--stride N` | 1048576 | requests between cursor publications. Larger is cheaper but coarser: a consumer cannot see data the reader has not published, so a very large stride leaves consumers idle in bursts. |
| `--read-batch N` | 8192 | requests the reader decodes per call. |
| `--batch N` | 32768 | batch depth for `batch-barrier`, matching libCacheSim's `queue_depth`. |

## Correctness

`tests/replay_test.cpp` checks the property that matters for all four: every
consumer sees exactly the trace, in order, once. The trace used has object ids
`0, 1, 2, ...` so a reordering, duplicate or gap is visible immediately, which
a realistic trace would hide.

The ring is then pushed into the states that are easy to get wrong: a ring far
smaller than the trace so it wraps a hundred times, consumers deliberately
running at different speeds so the reader has to wait on the slowest (the test
asserts the reader *did* stall, or it is not exercising what it claims), a
stride larger than the ring so the clamp is exercised, a single consumer, and
more consumers than cores.

The suite also runs clean under ThreadSanitizer:

```bash
g++ -std=c++20 -O1 -g -fsanitize=thread -Iinclude -Ithird_party -I. \
    tests/replay_test.cpp src/internal/sim/replay.cpp src/internal/trace/*.cpp \
    src/internal/cache/cache.cpp src/internal/eviction/policy_registry.cpp \
    -DCACHESIM_HAVE_ZSTD=1 -lzstd -o replay_tsan && ./replay_tsan
```
