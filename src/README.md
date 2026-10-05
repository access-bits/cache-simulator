# `src/`

Out-of-line definitions, mirroring `include/`. Everything else is header-only.

| File | Contents |
|---|---|
| `internal/cache/cache.cpp` | `Cache`'s cold paths: the miss branch, the eviction loop, size changes, explicit removal, and every error. |
| `internal/eviction/policy_registry.cpp` | The registry, and `registerBuiltinPolicies()` — the one list of what ships. |
| `internal/trace/byte_source.cpp` | `MmapSource`, `FileSource`, `ZstdSource`, `openByteSource`. |
| `internal/trace/record_layout.cpp` | Field sizes, format-character mapping, and the per-format layout presets. |
| `internal/trace/trace_format.cpp` | Format names, parsing, auto-detection. |

## Why these and not others

Three reasons a definition lives here rather than in a header:

**It is cold.** `Cache::access` is inline so the compiler can see the hit path
at the replay loop's call site. Its miss path, eviction loop and error paths
are `CACHESIM_NOINLINE` here — inlined, they would blow the loop's instruction
footprint for code that runs on a minority of requests, and that would make
inlining the hit path a net loss.

**It pulls in a heavy or platform-specific header.** `byte_source.cpp` is the
only file that includes `<sys/mman.h>` and `<zstd.h>`. `ZstdSource` stores its
stream as an opaque `void*` so that `zstd.h` stays out of the public header
and callers do not inherit the dependency — which is also what lets the
no-zstd build compile the same header.

**It is a list that should exist once.** `registerBuiltinPolicies()` is the
single place that knows which policies ship, so adding one is a one-line
change in a file whose only job is that list.

## What is deliberately *not* here

The data structures and the policies are templates or small enough to inline,
and a policy's whole body is usually a handful of calls into a `Queue`. Giving
them translation units would cost a call through a non-inlinable boundary on
the hottest path in the program for no benefit.
