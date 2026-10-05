#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "internal/request/request.hpp"
#include "internal/trace/trace_format.hpp"

namespace cachesim {

// How to read one trace: where it is, what shape it is, and what to do to
// the requests on the way out.
struct TraceSpec {
  std::string path;
  TraceFormat format = TraceFormat::kAuto;

  // Format-specific reader options as "key=value" pairs, using libCacheSim's
  // key names so existing configs carry over: obj-id-col, time-col,
  // obj-size-col, next-access-vtime-col, op-col, cpu-col, delimiter,
  // has-header, format, obj-id-is-num, page-shift, block-size.
  std::string params;

  // Stop after this many requests; negative means the whole trace. Counted
  // after sampling and filtering, i.e. it bounds the requests the cache
  // actually sees.
  std::int64_t num_req = -1;

  // Object sampling: keep a 1-in-N subset of *objects* (not requests), so
  // that every request for a kept object is kept and the reuse structure
  // survives. 1.0 means no sampling.
  double sample_ratio = 1.0;

  // Salt for the sampling hash, so several independent subsets can be drawn
  // from one trace.
  std::uint64_t sample_salt = 0;

  // Treat every object as size 1, making capacity an object count. This is
  // libCacheSim's global `ignore_obj_size`, and it is the right mode for
  // memory traces where every "object" is one page.
  bool ignore_obj_size = false;

  // Drop requests whose object size is 0. libCacheSim does this for several
  // formats by default: a zero-size object occupies nothing, so it can never
  // trigger eviction and an unbounded number of them can accumulate.
  bool skip_zero_size = true;
};

// A source of requests.
//
// The interface is batch-first on purpose. Pulling one request at a time
// through a virtual call puts an indirect call on the hot path for work that
// is often a single 24-byte memcpy; a batch of a few thousand amortizes that
// to nothing and lets the decoder run as a tight loop over mapped pages. The
// single-request next() is a convenience for tests and for code that is not
// in a hot loop.
class ITraceReader {
 public:
  virtual ~ITraceReader() = default;

  // Fills up to `count` requests and returns how many were written. A short
  // return means the trace is exhausted; 0 means it is.
  virtual std::size_t nextBatch(Request* out, std::size_t count) = 0;

  bool next(Request& out) { return nextBatch(&out, 1) == 1; }

  // Rewinds to the first request. Used to replay a trace more than once.
  virtual void reset() = 0;

  [[nodiscard]] virtual const std::string& path() const = 0;
  [[nodiscard]] virtual TraceFormat format() const = 0;

  // Requests in the trace, if it can be known cheaply (a fixed-record file's
  // size divided by its record size, or an lcs header's own count); 0 if not
  // (a compressed or text trace). Used to pre-size buffers and to report
  // progress, never for correctness.
  [[nodiscard]] virtual std::uint64_t estimatedRequests() const { return 0; }

  // Fraction of the trace consumed, in [0, 1]; 0 if unknown.
  [[nodiscard]] virtual double progress() const { return 0.0; }

  // Whether this trace carries next_access_vtime. Lets a run fail at startup
  // rather than on the first Belady request.
  [[nodiscard]] virtual bool hasOracle() const { return false; }
};

// Opens a trace according to `spec`, detecting the format if it is kAuto and
// applying sampling, size handling and the request limit. Throws TraceError
// if the file cannot be read and ConfigError if the spec does not describe a
// readable trace.
std::unique_ptr<ITraceReader> openTrace(const TraceSpec& spec);

// Reads the whole trace into memory. Returns the requests in order.
//
// The point is parameter sweeps. When fifty configurations replay the same
// trace, decoding it fifty times is fifty times the parse work and fifty
// independent walks over the file; decoding it once into a flat array lets
// every worker stream the same read-only memory with no parsing, no
// synchronization and no page faults after the first pass. At 32 bytes per
// request that is 32 GB for a billion requests, so the runner decides
// between this and per-worker streaming based on a memory budget — see
// TraceSource.
std::vector<Request> materializeTrace(const TraceSpec& spec, std::uint64_t max_requests = 0);

}  // namespace cachesim
