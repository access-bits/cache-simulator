#pragma once

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cachesim {

[[nodiscard]] inline std::string_view trim(std::string_view s) noexcept {
  const auto is_space = [](char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
  };
  while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
  while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
  return s;
}

[[nodiscard]] inline std::string toLower(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

// Case-insensitive compare. Used for every user-facing name (policy names,
// trace types, parameter keys) so that "LRU", "lru" and "Lru" all work —
// libCacheSim's YAML files mix all three, sometimes within one file.
[[nodiscard]] inline bool iequals(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    char ca = a[i], cb = b[i];
    if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
    if (ca != cb) return false;
  }
  return true;
}

// Normalizes a user-supplied identifier for lookup: lowercase, and '_'/'-'/' '
// all collapse away entirely. This is what makes "lru-k", "LRU_K" and "lruk"
// the same policy name, and "obj-id-col" the same key as "obj_id_col".
[[nodiscard]] inline std::string normalizeKey(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '_' || c == '-' || c == ' ') continue;
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    out.push_back(c);
  }
  return out;
}

[[nodiscard]] inline std::vector<std::string_view> split(std::string_view s, char delim) {
  std::vector<std::string_view> out;
  std::size_t start = 0;
  for (std::size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || s[i] == delim) {
      out.push_back(s.substr(start, i - start));
      start = i + 1;
    }
  }
  return out;
}

[[nodiscard]] inline std::optional<std::int64_t> parseInt(std::string_view s) {
  s = trim(s);
  if (s.empty()) return std::nullopt;
  bool neg = false;
  std::size_t i = 0;
  if (s[0] == '+' || s[0] == '-') {
    neg = s[0] == '-';
    i = 1;
  }
  // Accept 0x-prefixed hex: memory traces frequently quote raw addresses.
  int base = 10;
  if (i + 1 < s.size() && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
    base = 16;
    i += 2;
  }
  if (i >= s.size()) return std::nullopt;
  std::uint64_t value = 0;
  for (; i < s.size(); ++i) {
    const char c = s[i];
    int digit;
    if (c >= '0' && c <= '9') {
      digit = c - '0';
    } else if (base == 16 && c >= 'a' && c <= 'f') {
      digit = c - 'a' + 10;
    } else if (base == 16 && c >= 'A' && c <= 'F') {
      digit = c - 'A' + 10;
    } else {
      return std::nullopt;
    }
    value = value * static_cast<std::uint64_t>(base) + static_cast<std::uint64_t>(digit);
  }
  const auto signed_value = static_cast<std::int64_t>(value);
  return neg ? -signed_value : signed_value;
}

[[nodiscard]] inline std::optional<double> parseDouble(std::string_view s) {
  s = trim(s);
  if (s.empty()) return std::nullopt;
  const std::string owned(s);
  char* end = nullptr;
  const double value = std::strtod(owned.c_str(), &end);
  if (end == owned.c_str() || *end != '\0') return std::nullopt;
  return value;
}

// libCacheSim's YAML convention: true/yes/1 are true, everything else false.
// We additionally accept "on" and reject nothing, to stay permissive.
[[nodiscard]] inline bool parseBool(std::string_view s) {
  const std::string v = toLower(trim(s));
  return v == "true" || v == "yes" || v == "1" || v == "on";
}

// Parses a human cache size: a plain byte count, or a number with a unit
// suffix. Both SI-style ("100MB", "100m") and IEC ("100MiB") spellings are
// accepted and both mean powers of 1024 — this matches libCacheSim, whose
// YAML docs list "100MB" and "100MiB" as the same thing, so configs carry
// over unchanged.
[[nodiscard]] inline std::optional<std::uint64_t> parseSize(std::string_view s) {
  s = trim(s);
  if (s.empty()) return std::nullopt;
  std::size_t digits_end = 0;
  while (digits_end < s.size() && ((s[digits_end] >= '0' && s[digits_end] <= '9') ||
                                   s[digits_end] == '.')) {
    ++digits_end;
  }
  if (digits_end == 0) return std::nullopt;
  const auto number = parseDouble(s.substr(0, digits_end));
  if (!number || *number < 0) return std::nullopt;

  const std::string unit = toLower(trim(s.substr(digits_end)));
  std::uint64_t multiplier = 1;
  if (unit.empty() || unit == "b") {
    multiplier = 1;
  } else if (unit == "k" || unit == "kb" || unit == "kib") {
    multiplier = 1024ULL;
  } else if (unit == "m" || unit == "mb" || unit == "mib") {
    multiplier = 1024ULL * 1024;
  } else if (unit == "g" || unit == "gb" || unit == "gib") {
    multiplier = 1024ULL * 1024 * 1024;
  } else if (unit == "t" || unit == "tb" || unit == "tib") {
    multiplier = 1024ULL * 1024 * 1024 * 1024;
  } else {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(*number * static_cast<double>(multiplier));
}

// A bag of "key=value" pairs, which is how both libCacheSim and this
// simulator pass algorithm- and reader-specific options through a single
// string field in the YAML ("sampling-period=1000,seed=42").
//
// Separators accepted: ',' and ';' between pairs, '=' or ':' between key and
// value. Keys are matched with normalizeKey(), so "sampling_period",
// "sampling-period" and "SamplingPeriod" are one key. A bare key with no
// value is recorded with an empty value, which get<bool> reads as true so
// that "...,verbose,..." works as a flag.
class ParamMap {
 public:
  ParamMap() = default;
  explicit ParamMap(std::string_view text) { parse(text); }

  void parse(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
      while (i < text.size() && (text[i] == ',' || text[i] == ';' || text[i] == ' ')) ++i;
      const std::size_t start = i;
      while (i < text.size() && text[i] != ',' && text[i] != ';') ++i;
      std::string_view item = trim(text.substr(start, i - start));
      if (item.empty()) continue;
      const std::size_t eq = item.find_first_of("=:");
      if (eq == std::string_view::npos) {
        entries_.emplace_back(normalizeKey(item), std::string{});
      } else {
        entries_.emplace_back(normalizeKey(item.substr(0, eq)),
                              std::string(trim(item.substr(eq + 1))));
      }
    }
  }

  [[nodiscard]] bool has(std::string_view key) const {
    return find(key) != nullptr;
  }

  [[nodiscard]] std::string getString(std::string_view key, std::string fallback = {}) const {
    const std::string* v = find(key);
    return v != nullptr ? *v : fallback;
  }

  [[nodiscard]] std::int64_t getInt(std::string_view key, std::int64_t fallback) const {
    const std::string* v = find(key);
    if (v == nullptr) return fallback;
    const auto parsed = parseInt(*v);
    return parsed ? *parsed : fallback;
  }

  [[nodiscard]] double getDouble(std::string_view key, double fallback) const {
    const std::string* v = find(key);
    if (v == nullptr) return fallback;
    const auto parsed = parseDouble(*v);
    return parsed ? *parsed : fallback;
  }

  [[nodiscard]] std::uint64_t getSize(std::string_view key, std::uint64_t fallback) const {
    const std::string* v = find(key);
    if (v == nullptr) return fallback;
    const auto parsed = parseSize(*v);
    return parsed ? *parsed : fallback;
  }

  [[nodiscard]] bool getBool(std::string_view key, bool fallback) const {
    const std::string* v = find(key);
    if (v == nullptr) return fallback;
    if (v->empty()) return true;  // bare flag
    return parseBool(*v);
  }

  // Every key the caller never asked about. Policies and readers use this to
  // reject typos loudly instead of silently running with a default — a
  // misspelled "sampling_perod=1000" that silently does nothing is the kind
  // of thing that quietly invalidates a week of sweep results.
  [[nodiscard]] std::vector<std::string> unusedKeys() const {
    std::vector<std::string> out;
    for (const auto& [key, value] : entries_) {
      (void)value;
      if (!used_.contains(key)) out.push_back(key);
    }
    return out;
  }

  [[nodiscard]] bool empty() const { return entries_.empty(); }

 private:
  const std::string* find(std::string_view key) const {
    const std::string norm = normalizeKey(key);
    used_.insert(norm);
    for (const auto& [k, v] : entries_) {
      if (k == norm) return &v;
    }
    return nullptr;
  }

  std::vector<std::pair<std::string, std::string>> entries_;
  // Mutable because reading a key is how a caller declares it supported;
  // that bookkeeping is logically const with respect to the parsed content.
  mutable std::set<std::string> used_;
};

}  // namespace cachesim
