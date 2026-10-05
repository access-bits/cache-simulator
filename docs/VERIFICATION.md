# Verifying against libCacheSim

Two simulators replaying the same trace against the same policy at the same
capacity are computing the same deterministic function. The number of misses
is not a modelling choice — it is determined by the trace. So if they
disagree, one of them is wrong, and that makes the comparison a test rather
than a benchmark.

It is the strongest test in this repository. The internal cross-validation in
`tests/policy_test.cpp` checks each policy against a reference written by the
same person who wrote the policy, from the same reading of the same paper; a
misreading shows up in both. libCacheSim is an independent implementation by
other people, so it does not share that failure mode.

## Running it

```bash
# 1. Build libCacheSim, and patch it to print raw counts (see below).
# 2. Build this simulator.
cmake -S . -B build && cmake --build build -j

# 3. Make a trace both can read.
./build/tools/gen_trace /tmp/zipf.oracleGeneral --requests 100000000 --objects 10000000

# 4. Compare.
python3 tools/compare_libcachesim.py /tmp/zipf.oracleGeneral \
    --policy LRU --policy LFU --policy Belady --policy FIFO --policy Clock \
    --policy Sieve --policy ARC --policy S3FIFO --policy TwoQ \
    --size 100000 --size 1000000
```

### Why libCacheSim has to be patched

Its result line prints the miss ratio to four decimal places. At a billion
requests that is 50,000 misses of slack — enough to hide every bug found
below. In `libCacheSim/bin/cachesim/main.c`, extend the result line's
`snprintf` with the raw counters:

```c
", n_req %lld, n_miss %lld, n_req_byte %lld, n_miss_byte %lld",
..., (long long)result[i].n_req, (long long)result[i].n_miss,
(long long)result[i].n_req_byte, (long long)result[i].n_miss_byte
```

and rebuild the `cachesim` target. `compare_libcachesim.py --patch-note`
prints this. Without the patch the script still runs, compares the
four-decimal ratios, and says that it is doing so.

## What agrees

Nine policies produce **byte-identical miss counts**, verified at 200,000
requests across many capacities and at 100,000,000 requests:

| policy | status |
|---|---|
| LRU | exact |
| LFU | exact |
| Belady | exact |
| FIFO | exact |
| Clock | exact |
| Sieve | exact |
| ARC | exact |
| S3FIFO | exact |
| 2Q | exact |

At 100M requests, cache size 1,000,000 objects:

```
policy    cache_size        n_req     libCacheSim       cachesim    delta
LRU          100000     100000000        46661808       46661808        0
LRU         1000000     100000000        32567365       32567365        0
LFU          100000     100000000        39412949       39412949        0
LFU         1000000     100000000        28373280       28373280        0
Belady       100000     100000000        34480882       34480882        0
Belady      1000000     100000000        22886270       22886270        0
```

## What does not, and why

Three policies differ, by implementation convention rather than by
correctness. Each is cross-validated against its paper's definition in
`tests/policy_test.cpp`; where the two implementations make different choices,
this is which choice each made.

**SLRU** — about 0.3% apart. Both use four equal segments; the promotion and
demotion rules differ.

**LFU-DA** — libCacheSim ages by adding the *current minimum frequency* to
each object on a hit. Arlitt et al. define the key as
`reference_count + L`, where `L` rises to the key of each evicted object.
This implementation follows the paper.

**W-TinyLFU** — different window and main-region conventions.

**MRU, LRU-K** — libCacheSim has no equivalent to compare against.

Closing these three means choosing which implementation to match, and for all
three the papers are clearer than either codebase. They are left as they are,
documented, rather than changed to agree with another tool's reading.

## Four bugs this found

All four were in this simulator, and none of them would have crashed
anything. Each would have produced a quietly wrong hit ratio — the failure
mode that survives into a published number.

**1. ARC's target `p` has to be real-valued.** The paper's adaptation step is
`|B2|/|B1|` or its reciprocal — a ratio, and almost never a whole number. `p`
was an integer, so every adaptation truncated. The error does not cancel,
because truncation always moves `p` back towards where it already was, so `p`
systematically under-adapted. Worth about 0.3% of hit ratio. libCacheSim keeps
`p` as a `double`; with that fixed the two agree request for request.

**2. ARC's one un-ghosted eviction was missing.** In case IV with T1 filling
the whole cache, B1 is empty and the `|T1| + |B1| <= c` bound can only be
relieved by shrinking T1 — so the object that leaves is *discarded* rather
than recorded in B1. Ghosting it would push the sum straight back to the bound
it was just brought under. Getting this wrong changes B1's contents, and with
them every subsequent adaptation of `p`.

**3. S3FIFO's promotion threshold is 2, not 1.** The paper promotes out of the
small queue on `freq > 1` — an object must be requested *twice* while in S.
Being requested once is exactly what a one-hit-wonder does, so a threshold of
1 measurably weakens the filter the policy exists to provide.

**4. S3FIFO needed the warm-up rule.** Until the first eviction the cache is
not full, so the engine never asks for a victim, and every object admitted
during the initial fill piled into S — leaving M empty and S many times over
its 10% share, so the first evictions had to drain S all the way back. The
paper's insert is unconditional and does not mention this; libCacheSim does
it, and it is clearly the intended steady state.

Two of the four (ARC's integer `p`, S3FIFO's threshold) are the kind of thing
that reads as correct in review and is only visible against a second
implementation.
