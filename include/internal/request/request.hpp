#pragma once

#include <cstdint>
#include <limits>

namespace cachesim {

// What kind of access this is. Block traces distinguish read from write;
// key-value traces distinguish get/set/add/delete. Policies are free to
// ignore it entirely (all the classical ones do), but a trace reader that
// drops the information cannot get it back, and some studies need it (e.g.
// counting write amplification, or treating a delete as an invalidation).
enum class Op : std::uint8_t {
  kUnknown = 0,
  kRead = 1,
  kWrite = 2,
  kGet = 3,
  kGets = 4,
  kSet = 5,
  kAdd = 6,
  kCas = 7,
  kReplace = 8,
  kAppend = 9,
  kPrepend = 10,
  kDelete = 11,
  kIncr = 12,
  kDecr = 13,
  kInvalid = 255,
};

[[nodiscard]] inline const char* opName(Op op) {
  switch (op) {
    case Op::kUnknown: return "unknown";
    case Op::kRead: return "read";
    case Op::kWrite: return "write";
    case Op::kGet: return "get";
    case Op::kGets: return "gets";
    case Op::kSet: return "set";
    case Op::kAdd: return "add";
    case Op::kCas: return "cas";
    case Op::kReplace: return "replace";
    case Op::kAppend: return "append";
    case Op::kPrepend: return "prepend";
    case Op::kDelete: return "delete";
    case Op::kIncr: return "incr";
    case Op::kDecr: return "decr";
    case Op::kInvalid: return "invalid";
  }
  return "unknown";
}

// "This object is never requested again." Belady treats such an object as the
// best possible victim, so it has to sort above every real future time.
inline constexpr std::int64_t kNeverAgain = std::numeric_limits<std::int64_t>::max();

// "This trace does not carry look-ahead information." Distinct from
// kNeverAgain: a policy that needs the oracle must fail loudly on a
// non-oracle trace rather than silently behave as if every object were dead.
inline constexpr std::int64_t kNoOracle = -1;

// A single trace event.
//
// Exactly 32 bytes and trivially copyable, so two requests fit in a cache
// line and a batch of them can be memcpy'd or mmap'd into place. The layout
// is ordered big-to-small to leave no padding holes.
//
// On scope: this deliberately carries three things beyond the bare
// (id, size, time) triple that a pure LRU replay needs, because the
// alternative — a parallel side-channel array keyed by request index — costs
// a second random memory stream in the hot loop and has to be threaded
// through every reader, every policy and every plugin:
//
//   * next_access_vtime — the offline oracle. Belady and its variants cannot
//     exist without it, and it is a first-class column of libCacheSim's
//     oracleGeneral and lcs trace formats, so the reader has it in hand
//     already.
//   * cpu_id — which hardware thread issued the access. Carried by merged
//     multi-CPU memory traces and needed by the per-CPU TLB filter plugin.
//   * op — read/write/get/set/delete, as above.
//
// Everything genuinely policy- or plugin-specific (TTL, tenant, cost, ML
// feature vectors — the fields that made libCacheSim's request_t a kitchen
// sink) stays out. The test is whether a trace reader can produce it and more
// than one consumer needs it; these three pass, the rest do not.
struct Request {
  // Identity. For block traces a logical block address, for memory traces a
  // page number, for key-value traces a hash of the key.
  std::uint64_t obj_id = 0;

  // Wall-clock time from the trace, in whatever unit the trace uses
  // (seconds for most libCacheSim formats). Only used for time-based warmup
  // and for policies that age by real time; purely logical policies ignore it.
  std::int64_t clock_time = 0;

  // The logical request index at which this object is next requested, or
  // kNeverAgain if never, or kNoOracle if the trace does not provide it.
  std::int64_t next_access_vtime = kNoOracle;

  // Object size in bytes. uint32 caps a single object at 4 GiB, which is far
  // above anything in a cache trace; byte-capacity accounting itself is
  // 64-bit.
  std::uint32_t size = 1;

  // Issuing CPU for multi-core memory traces; 0 when the trace has no such
  // notion.
  std::uint16_t cpu_id = 0;

  Op op = Op::kUnknown;

  // Reserved so the struct stays exactly 32 bytes while leaving room for one
  // more flag byte without a layout change. Not interpreted by the engine.
  std::uint8_t flags = 0;

  [[nodiscard]] bool hasOracle() const { return next_access_vtime != kNoOracle; }
};

static_assert(sizeof(Request) == 32, "Request must stay 32 bytes: two per cache line");
static_assert(alignof(Request) == 8);

}  // namespace cachesim
