# `examples/`

## `replay.cpp`

A complete, runnable example of driving the simulator as a library. Until the
CLI exists, this is the way to run it.

```bash
cmake -S . -B build && cmake --build build -j
./build/examples/replay
./build/examples/replay --requests 5000000 --objects 500000 --seed 7
```

It builds a Zipf(1.0) trace in memory, fills in the oracle column that Belady
needs, replays it against every registered policy at four capacities
(0.1%, 1%, 5%, 10% of the object universe) and prints a miss-ratio table with
throughput.

The point is the five-step shape in `replay()`:

1. `PolicyConfig` — capacity, expected object count, algorithm parameters.
2. `CacheOptions` — how the cache itself behaves.
3. `Cache cache(options, PolicyRegistry::instance().create(name, config));`
4. `cache.access(req)` per request.
5. `cache.stats()`.

The reason the oracle column is computed by walking the trace backwards is
worth noting: `next_access_vtime` is the index at which each object is *next*
requested, which a real oracle trace format (`oracleGeneral`, `lcs`) stores as
a column, and which a synthetic trace has to derive. Belady refuses to run
without it rather than producing a quietly meaningless number.

Every object is given size 1 here, so capacity is an object count. The engine
is byte-based throughout — give requests real sizes and capacity means bytes,
and the byte miss ratio starts to mean something.
