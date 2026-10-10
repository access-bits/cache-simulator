# `tools/`

Trace generation, measurement, and the comparison harness against
libCacheSim. None of this is part of the library.

| Tool | What it does |
|---|---|
| `gen_trace.cpp` | Writes a synthetic Zipf trace in libCacheSim's oracleGeneral format |
| `bench.cpp` | Replays a trace against (policy, size) pairs and reports exact counts and throughput |
| `compare_libcachesim.py` | Runs both simulators on one trace file and diffs raw miss counts |
| `benchmark_vs_libcachesim.sh` | Times both, one configuration per process |
| `replay_bench.cpp` | Compares the four ways of delivering one trace to many concurrent simulations |

---

## `gen_trace`

```bash
./build/tools/gen_trace out.oracleGeneral --requests 1000000000 --objects 100000000 \
    --zipf 1.0 --seed 42 [--size-min 1 --size-max 1]
```

Writes 24-byte oracleGeneral records, so libCacheSim reads the output
unchanged. A billion requests is 24 GiB.

Two things in here are worth knowing about.

**The oracle column needs two passes.** `next_access_vtime` is the logical
index at which each object is *next* requested, which is only knowable once
the rest of the trace exists. Holding a billion requests in memory to work it
out would take 32 GB. So pass 1 generates ids and sizes forward with a
placeholder, and pass 2 walks the file *backwards* in 24 MiB chunks, keeping a
map of `obj_id -> index where it is next seen` and patching each record in
place. That map holds one entry per distinct **object**, not per request — 100
million objects is about 2.7 GB — which is what makes the whole thing fit.

**Zipf draws invert the CDF rather than searching a table.** A CDF over 100
million objects is 800 MB and a binary search through it is a guaranteed cache
miss per draw, which would make trace generation slower than the simulation it
feeds. Instead the continuous Zipf CDF is inverted directly: for exponent
`a != 1`,

```
rank(u) = ((1-u) + u * N^(1-a)) ^ (1/(1-a))
```

and for `a == 1` it reduces to `N^u`. Two transcendental calls per draw, no
memory traffic. The discretization error is concentrated in the few most
popular ranks, where the counts are enormous either way.

## `bench`

```bash
./build/tools/bench TRACE --type oracleGeneral \
    --policy LRU --policy LFU --size 100000 --size 1000000 \
    [--requests N] [--threads N] [--params "k=4"] [--stream] [--csv] [--repeat N]
```

By default the trace is decoded once into memory and every job replays the
same array, which isolates the simulator's own cost from trace decoding.
`--stream` replays straight from the file instead, which is what a trace too
large to hold in memory needs — and what to use when comparing against a tool
that also streams.

`--threads N` runs the (policy, size) pairs concurrently. The entire work
distribution is one atomic counter: each worker takes the next job and runs it
to completion. There is no shared mutable state during replay, so no locks and
no barriers, and the reported speedup should be close to linear until memory
bandwidth runs out.

`--repeat N` runs N rounds and reports the last, which is how to get a number
that is not measuring a cold page cache.

## `compare_libcachesim.py`

See [`../docs/VERIFICATION.md`](../docs/VERIFICATION.md) for what this is for,
what currently agrees, and the four bugs it found.

```bash
python3 tools/compare_libcachesim.py TRACE --policy LRU --policy Belady \
    --size 100000 --size 1000000 [--requests N] [--ignore-obj-size]
python3 tools/compare_libcachesim.py --patch-note    # how to get exact counts out of libCacheSim
```

It compares raw miss **counts**, not ratios. libCacheSim prints four decimal
places, which at a billion requests leaves 50,000 misses of slack — enough to
hide every bug the comparison actually found.

## `benchmark_vs_libcachesim.sh`

```bash
./tools/benchmark_vs_libcachesim.sh TRACE REQUESTS CACHE_SIZE [POLICY...]
```

One configuration per process for each tool, so the number is
single-configuration end-to-end throughput including trace decode. It reads
the portion of the trace both tools will touch before timing anything, so
whichever runs second does not get a free warm page cache — and it checks that
the miss counts still match, because a performance number from two tools that
disagree is meaningless.

## `replay_bench`

```bash
./build/tools/replay_bench TRACE --type oracleGeneral --policy LRU \
    --size 100000 --size 1000000 [--strategy shared-ring] [--all] \
    [--ring-mb N] [--stride N] [--batch N] [--requests N]
```

`--all` runs every delivery strategy and checks that they produce identical
miss counts, because the trace delivered is the same trace. See
[`../docs/REPLAY.md`](../docs/REPLAY.md) for the designs, the measurements and
how to tune the ring.
