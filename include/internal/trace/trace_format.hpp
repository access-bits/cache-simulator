#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace cachesim {

// Trace formats this simulator can read. The names are libCacheSim's, so an
// existing YAML config's `trace.type` works unchanged.
enum class TraceFormat {
  kAuto,             // detect from magic bytes and extension
  kCsv,              // delimited text, column mapping in reader params
  kTxt,              // one object id per line
  kBinary,           // fixed-size records, layout from a format string
  kOracleGeneral,    // libCacheSim oracleGeneral: 24-byte records with look-ahead
  kLcs,              // libCacheSim native: 8 KiB header, versioned records
  kVscsi,            // VSCSI block traces, v1 and v2
  kTwrBin,           // Twitter key-value binary
  kMergedTrace,      // multi-CPU merged memory trace, zstd batches + .meta
};

[[nodiscard]] std::string_view traceFormatName(TraceFormat format);

// Parses a format name, accepting libCacheSim's spellings and the obvious
// variants (case-insensitive, '-'/'_' ignored). Returns kAuto for an empty
// name. Throws ConfigError on an unrecognized one, listing what is accepted.
[[nodiscard]] TraceFormat parseTraceFormat(std::string_view name);

[[nodiscard]] std::vector<std::string> traceFormatNames();

// Guesses the format of a file from its first bytes and its name. Used when
// `trace.type` is absent. Returns kAuto if nothing matches, which the caller
// must report rather than guess at.
[[nodiscard]] TraceFormat detectTraceFormat(const std::string& path);

}  // namespace cachesim
