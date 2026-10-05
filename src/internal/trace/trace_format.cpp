#include "internal/trace/trace_format.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cstring>

#include "internal/common/error.hpp"
#include "internal/common/string_util.hpp"
#include "internal/trace/record_layout.hpp"

namespace cachesim {
namespace {

struct FormatName {
  TraceFormat format;
  const char* canonical;
  // Extra spellings accepted; matched after normalizeKey(), so case and
  // '-'/'_' do not matter.
  std::array<const char*, 4> aliases;
};

// Canonical names and the spellings libCacheSim's configs use.
constexpr std::array kFormatNames = std::to_array<FormatName>({
    {TraceFormat::kCsv, "csv", {nullptr, nullptr, nullptr, nullptr}},
    {TraceFormat::kTxt, "txt", {"plain", "text", nullptr, nullptr}},
    {TraceFormat::kBinary, "binary", {"bin", nullptr, nullptr, nullptr}},
    {TraceFormat::kOracleGeneral,
     "oracleGeneral",
     {"oracleGeneralBin", "oracle", nullptr, nullptr}},
    {TraceFormat::kLcs, "lcs", {nullptr, nullptr, nullptr, nullptr}},
    {TraceFormat::kVscsi, "vscsi", {nullptr, nullptr, nullptr, nullptr}},
    {TraceFormat::kTwrBin, "twrBin", {"twr", "twitter", nullptr, nullptr}},
    {TraceFormat::kMergedTrace, "mergedTrace", {"merged", nullptr, nullptr, nullptr}},
});

}  // namespace

std::string_view traceFormatName(TraceFormat format) {
  if (format == TraceFormat::kAuto) return "auto";
  for (const FormatName& entry : kFormatNames) {
    if (entry.format == format) return entry.canonical;
  }
  return "unknown";
}

std::vector<std::string> traceFormatNames() {
  std::vector<std::string> out;
  out.reserve(kFormatNames.size());
  for (const FormatName& entry : kFormatNames) out.emplace_back(entry.canonical);
  return out;
}

TraceFormat parseTraceFormat(std::string_view name) {
  const std::string key = normalizeKey(name);
  if (key.empty() || key == "auto") return TraceFormat::kAuto;
  for (const FormatName& entry : kFormatNames) {
    if (normalizeKey(entry.canonical) == key) return entry.format;
    for (const char* alias : entry.aliases) {
      if (alias != nullptr && normalizeKey(alias) == key) return entry.format;
    }
  }
  std::string available;
  for (const std::string& candidate : traceFormatNames()) {
    if (!available.empty()) available += ", ";
    available += candidate;
  }
  throw ConfigError("unknown trace type '" + std::string(name) + "'; available: " + available);
}

TraceFormat detectTraceFormat(const std::string& path) {
  // The lcs format is the only one of these with a magic number, so it is
  // the only one that can be identified with certainty. Everything else is
  // inferred from the file's name, which is why `trace.type` exists.
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd >= 0) {
    std::uint64_t magic = 0;
    const ssize_t got = ::read(fd, &magic, sizeof(magic));
    ::close(fd);
    if (got == static_cast<ssize_t>(sizeof(magic)) && magic == 0x123456789abcdef0ULL) {
      return TraceFormat::kLcs;
    }
  }

  const std::string lowered = toLower(path);
  const auto ends_with = [&](std::string_view suffix) {
    return lowered.size() >= suffix.size() &&
           lowered.compare(lowered.size() - suffix.size(), suffix.size(), suffix) == 0;
  };
  // Strip a compression suffix first: "trace.oracleGeneral.zst" should be
  // recognized by the part that says what the decompressed bytes are.
  std::string stem = lowered;
  for (std::string_view suffix : {".zst", ".zstd"}) {
    if (stem.size() >= suffix.size() &&
        stem.compare(stem.size() - suffix.size(), suffix.size(), suffix) == 0) {
      stem.resize(stem.size() - suffix.size());
      break;
    }
  }
  const auto stem_has = [&](std::string_view needle) {
    return stem.find(needle) != std::string::npos;
  };

  if (stem_has("oraclegeneral") || stem_has("oracle")) return TraceFormat::kOracleGeneral;
  if (stem_has("merged")) return TraceFormat::kMergedTrace;
  if (stem_has("vscsi")) return TraceFormat::kVscsi;
  if (stem_has(".lcs")) return TraceFormat::kLcs;
  if (stem_has("twr")) return TraceFormat::kTwrBin;
  if (ends_with(".csv") || stem.ends_with(".csv")) return TraceFormat::kCsv;
  if (stem.ends_with(".txt")) return TraceFormat::kTxt;
  return TraceFormat::kAuto;
}

}  // namespace cachesim
