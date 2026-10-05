# `include/internal/trace/`

Turning a file on disk into a stream of `Request` objects, as fast as the disk
and the format allow.

| Header | Role | State |
|---|---|---|
| `byte_source.hpp` | `MmapSource`, `FileSource`, `ZstdSource`, `openByteSource` | done |
| `record_layout.hpp` | `RecordLayout`, `decodeRecord`, and the per-format presets | done |
| `trace_format.hpp` | `TraceFormat`, name parsing, auto-detection | done |
| `trace_reader.hpp` | `TraceSpec`, `ITraceReader`, `openTrace`, `materializeTrace` | **interface only — `openTrace` and `materializeTrace` are declared, not yet implemented** |

---

## Byte sources

The interface is `peek(n)` / `consume(n)`, not `read(buffer, n)`:

```cpp
std::span<const std::byte> peek(std::size_t wanted);
void consume(std::size_t count);
```

A `read`-shaped interface forces a copy per call and a virtual call per
record. `peek` hands back a window into memory the source already owns, so a
fixed-record reader can take a megabyte at a time and iterate it with no
copies and no calls at all. For a memory-mapped file the window *is* the file,
and parsing a binary trace becomes a loop over mapped pages.

A short span means end-of-stream is within reach; an empty one means it has
been reached. Readers must handle both.

### `MmapSource`

The default for anything uncompressed. `mmap(PROT_READ, MAP_PRIVATE)` over the
whole file, with:

- `MADV_SEQUENTIAL` — the access pattern is one forward pass with every byte
  read once, so the kernel should read ahead aggressively and drop pages
  behind. Without it, a 100 GB trace evicts everything else on the machine
  from the page cache.
- `MADV_WILLNEED` — start the readahead now.

**This is also what removes the need for libCacheSim's shared reader thread
and batch barrier.** In a parameter sweep, every worker maps the same file
independently; the kernel backs all of those mappings with one set of
page-cache pages; no worker ever waits for another. The trace is read from
disk once however many configurations are running. libCacheSim instead has one
reader thread filling a bounded queue that all workers drain behind
semaphores, so every worker waits on the slowest at every batch boundary — and
that queue exists only because its reader is shared *mutable* state.

### `FileSource`

Buffered `read()` for anything that cannot be mapped — a pipe, a character
device, a file that grew since it was opened. `openByteSource` falls back to
it automatically if `mmap` fails.

### `ZstdSource`

Streaming decompression over another source. Trace files are routinely shipped
compressed, and decompressing to disk first can mean tens of times the
storage; streaming costs a memcpy per block, which is far cheaper.

Handles **concatenated frames**: `zstd` produces those when a file is appended
to, so a trace built incrementally is a sequence of frames rather than one.
The stream is re-initialized at each frame boundary and keeps going; a stream
that has genuinely ended returns an empty window from the source underneath.

If the build has no zstd (`CACHESIM_HAVE_ZSTD=0`), constructing one throws a
`TraceError` naming the file and saying what to install. Configuration still
succeeds without zstd, so the rest of the simulator builds and runs.

### `openByteSource`

Opens a path, maps it if possible, and wraps it in decompression if it looks
compressed. Compression is detected **by magic number first and extension
second**, because the extension says what someone named the file and the magic
says what it is. Both `0xFD2FB528` (a normal frame) and the skippable-frame
range `0x184D2A50..5F` count.

---

## Record layouts

One table-driven decoder serves every fixed-record format, rather than a
hand-written reader per format.

```cpp
struct RecordLayout {
  std::size_t record_size;
  Field obj_id, clock_time, obj_size, next_access_vtime, op, cpu,
        key_size, value_size;
  std::uint8_t  page_shift;     // obj_id >>= page_shift
  std::uint32_t block_size;     // obj_id /= block_size
  std::uint32_t time_divisor;   // VSCSI stores microseconds
  OpEncoding    op_encoding;    // direct, or SCSI command codes
  std::uint32_t default_size;   // for id-only traces
};

struct Field {
  FieldType     type;        // i8..u64, f32, f64
  std::uint16_t offset;
  std::uint8_t  bit_shift;   // for packed fields
  std::uint8_t  bit_width;   // 0 = the whole field
};
```

The bit range exists because real formats pack several values into one
integer: Twitter's binary format puts a 10-bit key size and a 22-bit value
size in one `uint32`, and lcs v2 packs an 8-bit op and a 24-bit tenant id
together.

This replaces roughly 400 lines of near-duplicate C in libCacheSim with a
struct and one loop, and it means a new fixed-record format is a table entry
rather than a new file.

Fields are loaded with `memcpy`, not a cast: trace records are packed and
therefore routinely misaligned. A constant-size `memcpy` compiles to a single
load and, unlike the cast, is not undefined behaviour.

### Presets

| Function | Format |
|---|---|
| `oracleGeneralLayout()` | 24 B: `u32` time, `u64` id, `u32` size, `i64` next-access |
| `lcsLayout(version)` | lcs v1–v8; v3+ widens size to 64 bits and adds TTL, v4+ append 1/2/4/8/16 feature words (feature[0] is read as the CPU id, which is the convention merged multi-CPU traces use) |
| `twrBinLayout()` | 20 B Twitter key-value, with key/value sizes and op/TTL bit-packed |
| `vscsiLayout(version)` | VSCSI v1 (32 B) and v2 (40 B) |
| `layoutFromFormatSpec(spec)` | a user-supplied Python-struct format string plus 1-based column indices, matching libCacheSim's `format=<IQIq` reader parameter |

`detectVscsiVersion` reads the version byte out of the first two records at
both candidate offsets; checking two records rather than one makes a
coincidence much less likely.

### A deliberate deviation from libCacheSim

libCacheSim's `read_data()` reads *every* field as signed, including `B`, `H`,
`I` and `Q`. That sign-extends an unsigned 32-bit field whose top bit is set.
For an object id either reading is injective, so hit ratios are unaffected;
for a size field the signed reading yields a negative number. Fields are read
as declared here. Format characters follow Python's `struct`: `b`/`B`/`c` are
one byte, `h`/`H` two, `i`/`I`/`l`/`L` four, `q`/`Q` eight, `f` four, `d`
eight, lowercase signed and uppercase unsigned.

---

## Formats and names

Names are libCacheSim's, so an existing config's `trace.type` works unchanged,
and parsing accepts the variants its own YAML files mix (case-insensitive,
`-` and `_` ignored).

| `trace.type` | Aliases | Records | Oracle | Reader |
|---|---|---|---|---|
| `oracleGeneral` | `oracleGeneralBin`, `oracle` | 24 B fixed | yes | layout ready |
| `lcs` | | 8 KiB header + versioned | yes | layout ready |
| `binary` | `bin` | user-described | optional | layout ready |
| `vscsi` | | 32 B or 40 B, auto-detected | no | layout ready |
| `twrBin` | `twr`, `twitter` | 20 B fixed | no | layout ready |
| `mergedTrace` | `merged` | 9 B, zstd batches | no | pending |
| `csv` | | delimited text | optional | pending |
| `txt` | `plain`, `text` | one id per line | no | pending |

`detectTraceFormat` is used when `trace.type` is absent. Only lcs has a magic
number (`0x123456789abcdef0`) and so is the only format that can be identified
with certainty; everything else is inferred from the filename, after stripping
a `.zst`/`.zstd` suffix so that `trace.oracleGeneral.zst` is recognized by the
part that describes the decompressed bytes.

### mergedTrace

9-byte packed records — `u64` virtual address, `u8` CPU id — in zstd batches,
with a sidecar `.meta` file listing `batch_index,entry_count,byte_size` and an
`address_mode` header.

Because each batch is a complete zstd frame and `ZstdSource` already handles
concatenated frames, the whole file streams as one logical sequence and the
`.meta` file is **not required** to read it. It is still read when present,
for `address_mode: virtual_addresses`, which means the ids are byte addresses
and need `>> 12` to become page numbers. libCacheSim requires the `.meta`
file, parses batch offsets from it, and runs a background thread doing
per-batch seek-and-decompress into ping-pong buffers; streaming is simpler and
has no synchronization in it.

---

## The reader interface

```cpp
class ITraceReader {
 public:
  virtual std::size_t nextBatch(Request* out, std::size_t count) = 0;
  bool next(Request& out);                      // convenience
  virtual void reset() = 0;
  virtual std::uint64_t estimatedRequests() const;
  virtual double progress() const;
  virtual bool hasOracle() const;
};
```

**Batch-first on purpose.** Pulling one request at a time through a virtual
call puts an indirect call on the hot path for work that is often a single
24-byte decode; a batch of a few thousand amortizes that to nothing and lets
the decoder run as a tight loop over mapped pages. `next()` is for tests and
for code that is not in a hot loop.

`hasOracle()` lets a run with Belady fail at startup rather than on the first
request.

### `TraceSpec`

| Field | Meaning |
|---|---|
| `path`, `format`, `params` | what to read and how |
| `num_req` | stop after this many requests, counted *after* sampling and filtering, so it bounds what the cache actually sees |
| `sample_ratio` | keep a 1-in-N subset of **objects**, not requests, so every request for a kept object is kept and the reuse structure survives |
| `sample_salt` | draw several independent subsets from one trace |
| `ignore_obj_size` | treat every object as size 1, making capacity an object count — the right mode for memory traces where each "object" is one page |
| `skip_zero_size` | drop zero-size requests, as libCacheSim does for several formats: a zero-size object occupies nothing, so it can never trigger eviction and an unbounded number can accumulate |

On sampling and the oracle: `next_access_vtime` refers to indices in the
*unsampled* trace, but sampling preserves the relative order of surviving
requests, so the ordering Belady needs is unaffected. The absolute values stop
being request indices, which matters only if you were reading them directly.

### `materializeTrace` and why it exists

Reading the whole trace into a flat array matters for parameter sweeps. When
fifty configurations replay the same trace, decoding it fifty times is fifty
times the parse work and fifty independent walks over the file; decoding it
once lets every worker stream the same read-only memory with no parsing, no
synchronization and no page faults after the first pass.

At 32 bytes per request that is 32 GB for a billion requests, so the runner
will choose between materializing and per-worker streaming on a memory budget
— materializing for text and compressed formats (where parsing is expensive)
when it fits, streaming from a shared mapping for uncompressed binary (where
parsing is nearly free and the page cache is already doing the sharing).

---

## What is left

`openTrace()` and `materializeTrace()` are declared and not yet defined, along
with the CSV, text and mergedTrace readers. The layouts and byte sources they
compose are done and tested. This is the next thing being built.
