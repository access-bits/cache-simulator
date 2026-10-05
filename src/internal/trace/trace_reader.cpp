#include "internal/trace/trace_reader.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "internal/common/error.hpp"
#include "internal/common/hash.hpp"
#include "internal/common/string_util.hpp"
#include "internal/trace/byte_source.hpp"
#include "internal/trace/record_layout.hpp"

namespace cachesim {
namespace {

using internal::decodeRecord;
using internal::IByteSource;
using internal::RecordLayout;

// How many records a fixed-record reader pulls from the byte source at a time.
// Large enough that the per-peek cost disappears, small enough to stay in L2
// (1 MiB of a 24-byte format).
constexpr std::size_t kSourceChunkRecords = 1u << 16;

// ---------------------------------------------------------------------------
// Fixed-size binary records
// ---------------------------------------------------------------------------

// Reads any format describable by a RecordLayout. This one class covers
// oracleGeneral, lcs v1-v8, Twitter binary, VSCSI v1/v2 and user-supplied
// format strings, because the only thing that differs between them is the
// table.
//
// The decode loop is the hottest code in trace ingestion, so it is written to
// be one pass over mapped memory: peek a chunk, decode straight into the
// caller's output array, consume. No copy of the record, no allocation, and
// one virtual call per batch rather than per request.
class BinaryTraceReader final : public ITraceReader {
 public:
  BinaryTraceReader(std::string path, TraceFormat format, std::unique_ptr<IByteSource> source,
                    RecordLayout layout)
      : path_(std::move(path)),
        format_(format),
        source_(std::move(source)),
        layout_(layout) {
    if (!layout_.valid()) {
      throw ConfigError("trace '" + path_ +
                        "' has no usable record layout: record size " +
                        std::to_string(layout_.record_size) + ", object id field " +
                        (layout_.obj_id.present() ? "present" : "missing"));
    }
  }

  std::size_t nextBatch(Request* out, std::size_t count) override {
    std::size_t produced = 0;
    while (produced < count) {
      const std::size_t wanted =
          std::min(count - produced, kSourceChunkRecords) * layout_.record_size;
      const std::span<const std::byte> window = source_->peek(wanted);
      const std::size_t available = window.size() / layout_.record_size;
      if (available == 0) break;  // end of stream, or a trailing partial record

      const std::byte* record = window.data();
      for (std::size_t i = 0; i < available; ++i, record += layout_.record_size) {
        decodeRecord(record, layout_, out[produced + i]);
      }
      source_->consume(available * layout_.record_size);
      produced += available;
    }
    return produced;
  }

  void reset() override { source_->reset(); }

  [[nodiscard]] const std::string& path() const override { return path_; }
  [[nodiscard]] TraceFormat format() const override { return format_; }

  [[nodiscard]] std::uint64_t estimatedRequests() const override {
    const std::uint64_t total = source_->totalBytes();
    return total > 0 ? total / layout_.record_size : 0;
  }

  [[nodiscard]] double progress() const override {
    const std::uint64_t total = source_->totalBytes();
    if (total == 0) return 0.0;
    return static_cast<double>(source_->position()) / static_cast<double>(total);
  }

  [[nodiscard]] bool hasOracle() const override {
    return layout_.next_access_vtime.present();
  }

 private:
  std::string path_;
  TraceFormat format_;
  std::unique_ptr<IByteSource> source_;
  RecordLayout layout_;
};

// ---------------------------------------------------------------------------
// Delimited text
// ---------------------------------------------------------------------------

// Column roles a CSV trace can declare, 1-based as libCacheSim's reader
// parameters express them. 0 means "not present".
struct CsvColumns {
  int obj_id = 0;
  int clock_time = 0;
  int obj_size = 0;
  int next_access_vtime = 0;
  int op = 0;
  int cpu = 0;
  int count = 0;  // the request is repeated this many times
};

Op parseOpToken(std::string_view token) {
  // Numeric first: several converters write the enum value directly.
  if (!token.empty() && token[0] >= '0' && token[0] <= '9') {
    const auto value = parseInt(token);
    if (value && *value >= 0 && *value <= 255) return static_cast<Op>(*value);
  }
  const std::string lowered = toLower(trim(token));
  if (lowered == "read" || lowered == "r" || lowered == "get") return Op::kRead;
  if (lowered == "write" || lowered == "w" || lowered == "set" || lowered == "put") {
    return Op::kWrite;
  }
  if (lowered == "delete" || lowered == "del" || lowered == "rm") return Op::kDelete;
  if (lowered == "gets") return Op::kGets;
  if (lowered == "add") return Op::kAdd;
  if (lowered == "cas") return Op::kCas;
  if (lowered == "replace") return Op::kReplace;
  if (lowered == "append") return Op::kAppend;
  if (lowered == "prepend") return Op::kPrepend;
  if (lowered == "incr") return Op::kIncr;
  if (lowered == "decr") return Op::kDecr;
  return Op::kUnknown;
}

// A text reader covering both CSV (column-mapped) and the one-id-per-line
// format, since the latter is just the former with a single column.
//
// Lines are found with memchr over the source's window and fields are split
// in place, so a line is parsed without being copied. The cost here is
// genuinely the integer parsing, which is why the sweep runner prefers to
// decode a text trace once into memory and share it rather than have every
// worker re-parse it.
class TextTraceReader final : public ITraceReader {
 public:
  TextTraceReader(std::string path, TraceFormat format, std::unique_ptr<IByteSource> source,
                  CsvColumns columns, char delimiter, bool has_header, bool obj_id_is_num)
      : path_(std::move(path)),
        format_(format),
        source_(std::move(source)),
        columns_(columns),
        delimiter_(delimiter),
        skip_header_(has_header),
        obj_id_is_num_(obj_id_is_num) {
    if (columns_.obj_id <= 0) {
      throw ConfigError("trace '" + path_ +
                        "' needs obj-id-col to say which column holds the object id");
    }
    max_column_ = std::max({columns_.obj_id, columns_.clock_time, columns_.obj_size,
                            columns_.next_access_vtime, columns_.op, columns_.cpu,
                            columns_.count});
  }

  std::size_t nextBatch(Request* out, std::size_t count) override {
    std::size_t produced = 0;
    while (produced < count) {
      // Emit any remaining repeats of the last line before reading more.
      if (pending_repeats_ > 0) {
        const std::size_t emit = std::min<std::size_t>(pending_repeats_, count - produced);
        for (std::size_t i = 0; i < emit; ++i) out[produced + i] = pending_;
        produced += emit;
        pending_repeats_ -= emit;
        continue;
      }
      std::string_view line;
      if (!nextLine(line)) break;
      if (skip_header_) {
        skip_header_ = false;
        continue;
      }
      if (line.empty() || line[0] == '#') continue;
      if (!parseLine(line, pending_, pending_repeats_)) continue;
      // Loop round; the repeat branch above emits it (count >= 1 always).
    }
    return produced;
  }

  void reset() override {
    source_->reset();
    pending_repeats_ = 0;
    skip_header_ = skip_header_initial_;
  }

  [[nodiscard]] const std::string& path() const override { return path_; }
  [[nodiscard]] TraceFormat format() const override { return format_; }

  [[nodiscard]] double progress() const override {
    const std::uint64_t total = source_->totalBytes();
    if (total == 0) return 0.0;
    return static_cast<double>(source_->position()) / static_cast<double>(total);
  }

  [[nodiscard]] bool hasOracle() const override { return columns_.next_access_vtime > 0; }

 private:
  // Finds the next newline-terminated run in the source's window, growing the
  // peek if a line spans the current one.
  bool nextLine(std::string_view& line) {
    std::size_t wanted = 64 * 1024;
    for (;;) {
      const std::span<const std::byte> window = source_->peek(wanted);
      if (window.empty()) return false;
      const char* begin = reinterpret_cast<const char*>(window.data());
      const void* found = std::memchr(begin, '\n', window.size());
      if (found != nullptr) {
        const std::size_t length = static_cast<const char*>(found) - begin;
        line = std::string_view(begin, length);
        source_->consume(length + 1);
        // Tolerate CRLF.
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        return true;
      }
      if (window.size() < wanted) {
        // No newline and the source is exhausted: a final line without a
        // terminator.
        line = std::string_view(begin, window.size());
        source_->consume(window.size());
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        return !line.empty();
      }
      wanted *= 2;  // the line is longer than the window
      if (wanted > (1u << 28)) {
        throw TraceError("trace '" + path_ + "' has a line longer than 256 MiB");
      }
    }
  }

  bool parseLine(std::string_view line, Request& out, std::size_t& repeats) {
    // Split into at most max_column_ fields; stop early rather than scanning
    // a wide row to its end.
    std::string_view fields[kMaxColumns];
    int field_count = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= line.size() && field_count < max_column_; ++i) {
      if (i == line.size() || line[i] == delimiter_) {
        fields[field_count++] = line.substr(start, i - start);
        start = i + 1;
      }
    }
    if (field_count < columns_.obj_id) return false;

    const auto field = [&](int column) -> std::string_view {
      if (column <= 0 || column > field_count) return {};
      return trim(fields[column - 1]);
    };

    const std::string_view id_token = field(columns_.obj_id);
    if (id_token.empty()) return false;
    if (obj_id_is_num_) {
      const auto parsed = parseInt(id_token);
      if (!parsed) return false;
      out.obj_id = static_cast<std::uint64_t>(*parsed);
    } else {
      out.obj_id = hashBytes(id_token);
    }

    out.clock_time = 0;
    if (columns_.clock_time > 0) {
      // Timestamps are sometimes fractional seconds; truncate rather than
      // reject, since only time-based warm-up reads this.
      if (const auto value = parseDouble(field(columns_.clock_time))) {
        out.clock_time = static_cast<std::int64_t>(*value);
      }
    }

    out.size = 1;
    if (columns_.obj_size > 0) {
      if (const auto value = parseInt(field(columns_.obj_size))) {
        out.size = static_cast<std::uint32_t>(std::max<std::int64_t>(0, *value));
      }
    }

    out.next_access_vtime = kNoOracle;
    if (columns_.next_access_vtime > 0) {
      if (const auto value = parseInt(field(columns_.next_access_vtime))) {
        out.next_access_vtime = (*value == -1) ? kNeverAgain : *value;
      }
    }

    out.op = columns_.op > 0 ? parseOpToken(field(columns_.op)) : Op::kUnknown;
    out.cpu_id = 0;
    if (columns_.cpu > 0) {
      if (const auto value = parseInt(field(columns_.cpu))) {
        out.cpu_id = static_cast<std::uint16_t>(*value);
      }
    }
    out.flags = 0;

    repeats = 1;
    if (columns_.count > 0) {
      if (const auto value = parseInt(field(columns_.count))) {
        repeats = static_cast<std::size_t>(std::max<std::int64_t>(1, *value));
      }
    }
    return true;
  }

  static constexpr int kMaxColumns = 64;

  std::string path_;
  TraceFormat format_;
  std::unique_ptr<IByteSource> source_;
  CsvColumns columns_;
  char delimiter_;
  bool skip_header_;
  bool skip_header_initial_ = false;
  bool obj_id_is_num_;
  int max_column_ = 1;
  Request pending_{};
  std::size_t pending_repeats_ = 0;
};

// ---------------------------------------------------------------------------
// Post-processing: sampling, size handling, request limits
// ---------------------------------------------------------------------------

// Wraps another reader and applies everything that is independent of the
// format: object-level sampling, forcing sizes to 1, dropping zero-size
// requests, and the request limit.
//
// A decorator rather than a flag inside each reader, so that a new format
// gets all of it for free and none of it can be forgotten.
class FilteringTraceReader final : public ITraceReader {
 public:
  FilteringTraceReader(std::unique_ptr<ITraceReader> inner, const TraceSpec& spec)
      : inner_(std::move(inner)),
        limit_(spec.num_req >= 0 ? static_cast<std::uint64_t>(spec.num_req) : 0),
        ignore_obj_size_(spec.ignore_obj_size),
        skip_zero_size_(spec.skip_zero_size),
        sample_salt_(spec.sample_salt) {
    if (spec.sample_ratio > 0.0 && spec.sample_ratio < 1.0) {
      sampling_ = true;
      // Keep an object when mix64(id ^ salt) < ratio * 2^64. libCacheSim uses
      // hash(id) % round(1/ratio) == 0, which is the same thing whenever
      // 1/ratio is an integer and strictly more general when it is not. The
      // two select different subsets regardless, because the hash functions
      // differ -- so a sampled run is comparable with another sampled run
      // from this simulator, not with libCacheSim's.
      threshold_ = static_cast<std::uint64_t>(
          spec.sample_ratio * 18446744073709551616.0);
      if (threshold_ == 0) threshold_ = 1;
    }
  }

  std::size_t nextBatch(Request* out, std::size_t count) override {
    if (limit_ > 0 && delivered_ >= limit_) return 0;
    if (limit_ > 0) count = std::min<std::size_t>(count, limit_ - delivered_);

    std::size_t produced = 0;
    while (produced < count) {
      // Pull into the caller's buffer and compact in place: with no sampling
      // and no zero sizes (the common case) nothing moves at all.
      const std::size_t got = inner_->nextBatch(out + produced, count - produced);
      if (got == 0) break;
      std::size_t kept = 0;
      for (std::size_t i = 0; i < got; ++i) {
        Request& req = out[produced + i];
        if (sampling_ && mix64(req.obj_id ^ sample_salt_) >= threshold_) continue;
        if (ignore_obj_size_) {
          req.size = 1;
        } else if (skip_zero_size_ && req.size == 0) {
          continue;
        }
        if (kept != i) out[produced + kept] = req;
        ++kept;
      }
      produced += kept;
      if (kept == 0 && got < count - produced) break;  // exhausted upstream
    }
    delivered_ += produced;
    return produced;
  }

  void reset() override {
    inner_->reset();
    delivered_ = 0;
  }

  [[nodiscard]] const std::string& path() const override { return inner_->path(); }
  [[nodiscard]] TraceFormat format() const override { return inner_->format(); }
  [[nodiscard]] std::uint64_t estimatedRequests() const override {
    const std::uint64_t upstream = inner_->estimatedRequests();
    std::uint64_t estimate = upstream;
    if (sampling_ && upstream > 0) {
      estimate = static_cast<std::uint64_t>(
          static_cast<double>(upstream) *
          (static_cast<double>(threshold_) / 18446744073709551616.0));
    }
    if (limit_ > 0) estimate = estimate == 0 ? limit_ : std::min(estimate, limit_);
    return estimate;
  }
  [[nodiscard]] double progress() const override {
    if (limit_ > 0) {
      return std::min(1.0, static_cast<double>(delivered_) / static_cast<double>(limit_));
    }
    return inner_->progress();
  }
  [[nodiscard]] bool hasOracle() const override { return inner_->hasOracle(); }

 private:
  std::unique_ptr<ITraceReader> inner_;
  std::uint64_t limit_ = 0;
  std::uint64_t delivered_ = 0;
  bool ignore_obj_size_ = false;
  bool skip_zero_size_ = true;
  bool sampling_ = false;
  std::uint64_t threshold_ = 0;
  std::uint64_t sample_salt_ = 0;
};

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

// Sentinel for "the user did not say", so that an explicit page-shift=0 can
// be told apart from no parameter at all.
constexpr int kPageShiftUnset = 255;

// Reads a mergedTrace's sidecar "<path>.meta" for its address_mode header and
// returns the page shift it implies: 12 for virtual_addresses (ids are byte
// addresses and have to be shifted to page numbers), 0 for page numbers.
//
// The sidecar is optional here. libCacheSim requires it, because it parses
// per-batch offsets from it and seeks to each batch; this reader streams the
// concatenated frames instead, so only the address mode is of interest, and a
// missing sidecar simply means "already page numbers".
std::uint8_t mergedTracePageShift(const std::string& trace_path) {
  std::FILE* meta = std::fopen((trace_path + ".meta").c_str(), "r");
  if (meta == nullptr) return 0;
  char line[512];
  std::uint8_t shift = 0;
  while (std::fgets(line, sizeof(line), meta) != nullptr) {
    const std::string_view text(line);
    if (text.rfind("# address_mode:", 0) == 0) {
      if (text.find("virtual_addresses") != std::string_view::npos) shift = 12;
      break;
    }
    // The CSV header ends the comment block.
    if (text.rfind("batch_index,", 0) == 0) break;
  }
  std::fclose(meta);
  return shift;
}

char parseDelimiter(const ParamMap& params, char fallback) {
  const std::string value = params.getString("delimiter");
  if (value.empty()) return fallback;
  // Accept the usual escapes, since a shell makes a literal tab awkward.
  if (value == "\\t" || value == "tab") return '\t';
  if (value == "\\s" || value == "space") return ' ';
  if (value == "\\n") return '\n';
  return value[0];
}

// Reads an lcs header and returns the record layout its version implies.
// The header is 8192 bytes: two magic numbers bracketing a version and a
// statistics block.
RecordLayout lcsLayoutFromHeader(IByteSource& source, const std::string& path) {
  constexpr std::size_t kHeaderBytes = 1024 * 8;
  constexpr std::uint64_t kMagic = 0x123456789abcdef0ULL;
  const std::span<const std::byte> header = source.peek(kHeaderBytes);
  if (header.size() < kHeaderBytes) {
    throw TraceError("trace '" + path + "' is too short to be an lcs trace (needs " +
                     std::to_string(kHeaderBytes) + " header bytes, has " +
                     std::to_string(header.size()) + ")");
  }
  std::uint64_t start_magic = 0;
  std::uint64_t version = 0;
  std::uint64_t end_magic = 0;
  std::memcpy(&start_magic, header.data(), sizeof(start_magic));
  std::memcpy(&version, header.data() + 8, sizeof(version));
  std::memcpy(&end_magic, header.data() + kHeaderBytes - 8, sizeof(end_magic));
  if (start_magic != kMagic || end_magic != kMagic) {
    throw TraceError("trace '" + path +
                     "' does not have lcs magic numbers at both ends of its header; "
                     "it is probably not an lcs trace");
  }
  source.consume(kHeaderBytes);
  return internal::lcsLayout(version);
}

std::unique_ptr<ITraceReader> openUnfiltered(const TraceSpec& spec) {
  if (spec.path.empty()) throw ConfigError("no trace path given");

  TraceFormat format = spec.format;
  if (format == TraceFormat::kAuto) {
    format = detectTraceFormat(spec.path);
    if (format == TraceFormat::kAuto) {
      throw ConfigError(
          "cannot tell what format trace '" + spec.path +
          "' is in. Set the trace type explicitly (oracleGeneral, lcs, csv, txt, "
          "binary, vscsi, twrBin, mergedTrace).");
    }
  }

  const ParamMap params(spec.params);
  std::unique_ptr<IByteSource> source = internal::openByteSource(spec.path);

  switch (format) {
    case TraceFormat::kOracleGeneral: {
      RecordLayout layout = internal::oracleGeneralLayout();
      layout.page_shift = static_cast<std::uint8_t>(params.getInt("page-shift", 0));
      layout.block_size = static_cast<std::uint32_t>(params.getInt("block-size", 0));
      return std::make_unique<BinaryTraceReader>(spec.path, format, std::move(source), layout);
    }
    case TraceFormat::kLcs: {
      RecordLayout layout = lcsLayoutFromHeader(*source, spec.path);
      layout.page_shift = static_cast<std::uint8_t>(params.getInt("page-shift", 0));
      layout.block_size = static_cast<std::uint32_t>(params.getInt("block-size", 0));
      return std::make_unique<BinaryTraceReader>(spec.path, format, std::move(source), layout);
    }
    case TraceFormat::kTwrBin: {
      RecordLayout layout = internal::twrBinLayout();
      return std::make_unique<BinaryTraceReader>(spec.path, format, std::move(source), layout);
    }
    case TraceFormat::kVscsi: {
      // The version lives in the records themselves, so it has to be sniffed
      // before a layout can be chosen.
      const std::span<const std::byte> probe = source->peek(40 * 2);
      const int version = internal::detectVscsiVersion(probe.data(), probe.size());
      if (version == 0) {
        throw TraceError("trace '" + spec.path +
                         "' does not look like a vscsi trace: neither the v1 nor the v2 "
                         "version byte is consistent across its first records");
      }
      RecordLayout layout = internal::vscsiLayout(version);
      layout.block_size = static_cast<std::uint32_t>(params.getInt("block-size", 0));
      return std::make_unique<BinaryTraceReader>(spec.path, format, std::move(source), layout);
    }
    case TraceFormat::kMergedTrace: {
      // 9-byte packed records: a virtual address and the CPU that issued it.
      RecordLayout layout;
      layout.record_size = 9;
      layout.obj_id = {internal::FieldType::kU64, 0, 0, 0};
      layout.cpu = {internal::FieldType::kU8, 8, 0, 0};
      layout.default_size = 1;
      // address_mode=virtual_addresses in the sidecar .meta means the ids are
      // byte addresses and have to be shifted to page numbers. The sidecar is
      // read when present but is not required: every batch is a complete zstd
      // frame, and the byte source already stitches concatenated frames, so
      // the whole file streams without the batch index libCacheSim needs.
      std::uint8_t page_shift =
          static_cast<std::uint8_t>(params.getInt("page-shift", kPageShiftUnset));
      if (page_shift == kPageShiftUnset) page_shift = mergedTracePageShift(spec.path);
      layout.page_shift = page_shift;
      return std::make_unique<BinaryTraceReader>(spec.path, format, std::move(source), layout);
    }
    case TraceFormat::kBinary: {
      internal::BinaryFormatSpec binary;
      binary.format = params.getString("format");
      binary.obj_id_col = static_cast<int>(params.getInt("obj-id-col", 0));
      binary.time_col = static_cast<int>(params.getInt("time-col", 0));
      binary.obj_size_col = static_cast<int>(params.getInt("obj-size-col", 0));
      binary.next_access_vtime_col =
          static_cast<int>(params.getInt("next-access-vtime-col", 0));
      binary.op_col = static_cast<int>(params.getInt("op-col", 0));
      binary.cpu_col = static_cast<int>(params.getInt("cpu-col", 0));
      RecordLayout layout = internal::layoutFromFormatSpec(binary);
      layout.page_shift = static_cast<std::uint8_t>(params.getInt("page-shift", 0));
      layout.block_size = static_cast<std::uint32_t>(params.getInt("block-size", 0));
      return std::make_unique<BinaryTraceReader>(spec.path, format, std::move(source), layout);
    }
    case TraceFormat::kCsv: {
      CsvColumns columns;
      columns.obj_id = static_cast<int>(params.getInt("obj-id-col", 0));
      columns.clock_time = static_cast<int>(params.getInt("time-col", 0));
      columns.obj_size = static_cast<int>(params.getInt("obj-size-col", 0));
      columns.next_access_vtime = static_cast<int>(params.getInt("next-access-vtime-col", 0));
      columns.op = static_cast<int>(params.getInt("op-col", 0));
      columns.cpu = static_cast<int>(params.getInt("cpu-col", 0));
      columns.count = static_cast<int>(params.getInt("cnt-col", 0));
      return std::make_unique<TextTraceReader>(
          spec.path, format, std::move(source), columns, parseDelimiter(params, ','),
          params.getBool("has-header", false), params.getBool("obj-id-is-num", true));
    }
    case TraceFormat::kTxt: {
      CsvColumns columns;
      columns.obj_id = static_cast<int>(params.getInt("obj-id-col", 1));
      columns.clock_time = static_cast<int>(params.getInt("time-col", 0));
      columns.obj_size = static_cast<int>(params.getInt("obj-size-col", 0));
      return std::make_unique<TextTraceReader>(
          spec.path, format, std::move(source), columns, parseDelimiter(params, ' '),
          params.getBool("has-header", false), params.getBool("obj-id-is-num", true));
    }
    case TraceFormat::kAuto:
      break;
  }
  throw ConfigError("trace format '" + std::string(traceFormatName(format)) +
                    "' has no reader in this build");
}

}  // namespace

std::unique_ptr<ITraceReader> openTrace(const TraceSpec& spec) {
  std::unique_ptr<ITraceReader> reader = openUnfiltered(spec);
  const bool needs_filtering = spec.num_req >= 0 || spec.ignore_obj_size ||
                               (spec.sample_ratio > 0.0 && spec.sample_ratio < 1.0) ||
                               spec.skip_zero_size;
  if (!needs_filtering) return reader;
  return std::make_unique<FilteringTraceReader>(std::move(reader), spec);
}

std::vector<Request> materializeTrace(const TraceSpec& spec, std::uint64_t max_requests) {
  std::unique_ptr<ITraceReader> reader = openTrace(spec);
  std::vector<Request> trace;

  // Pre-size from the reader's own estimate where it has one: for a
  // fixed-record file that is exact, which turns what would be a dozen
  // reallocations of a multi-gigabyte vector into one allocation.
  std::uint64_t estimate = reader->estimatedRequests();
  if (max_requests > 0 && (estimate == 0 || estimate > max_requests)) estimate = max_requests;
  if (estimate > 0) trace.reserve(static_cast<std::size_t>(estimate));

  constexpr std::size_t kBatch = 1u << 16;
  std::vector<Request> batch(kBatch);
  for (;;) {
    std::size_t wanted = kBatch;
    if (max_requests > 0) {
      const std::uint64_t remaining = max_requests - trace.size();
      if (remaining == 0) break;
      wanted = static_cast<std::size_t>(std::min<std::uint64_t>(wanted, remaining));
    }
    const std::size_t got = reader->nextBatch(batch.data(), wanted);
    if (got == 0) break;
    trace.insert(trace.end(), batch.begin(), batch.begin() + static_cast<std::ptrdiff_t>(got));
  }
  return trace;
}

}  // namespace cachesim
