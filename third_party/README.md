# `third_party/`

## `ankerl/unordered_dense.h`

[unordered_dense](https://github.com/martinus/unordered_dense) by Martin
Leitner-Ankerl, MIT licensed. Version 5.0.

**The only external dependency of the simulator** (zstd is optional and only
needed to read compressed traces). It is vendored rather than fetched so that
the build never needs the network — on a cluster node, in a container, or
years from now when a URL has moved, that is the difference between "builds"
and "does not build".

### Why not `std::unordered_map`

The `obj_id → CacheEntry*` lookup is the single hottest operation in the
simulator: every request starts with one. The C++ standard effectively
mandates that `std::unordered_map` be separately chained with stable element
addresses, which means a bucket array of pointers into nodes scattered across
the heap, and a node allocation per insert. On a miss-heavy trace that is a
pointer chase and an allocator round trip per request — two of the three
things this project exists to avoid.

`unordered_dense` is open-addressed with its values in one contiguous vector:
a lookup is a bucket probe and one dense-array access, and inserting does not
allocate per element. It is also well-benchmarked and small enough to read.

### Why its default hash is left alone

`ankerl::unordered_dense::hash<std::uint64_t>` forwards to `std::hash`, which
for an integer is the identity — and because it is not declared avalanching,
the map applies its own mixing step. So the default already mixes exactly
once, which is what a structured object id needs. Supplying a pre-mixed hash
would make it mix twice for no gain.

(`mix64` in `internal/common/hash.hpp` exists for the places that index
*without* a hash map: the frequency sketch's rows, and object-level trace
sampling.)
