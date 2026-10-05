#include "internal/trace/record_layout.hpp"

#include <string>

#include "internal/common/error.hpp"

namespace cachesim::internal {

std::size_t fieldTypeSize(FieldType type) {
  switch (type) {
    case FieldType::kNone: return 0;
    case FieldType::kI8:
    case FieldType::kU8: return 1;
    case FieldType::kI16:
    case FieldType::kU16: return 2;
    case FieldType::kI32:
    case FieldType::kU32:
    case FieldType::kF32: return 4;
    case FieldType::kI64:
    case FieldType::kU64:
    case FieldType::kF64: return 8;
  }
  return 0;
}

FieldType fieldTypeFromFormatChar(char c) {
  switch (c) {
    case 'b': case 'c': return FieldType::kI8;
    case 'B': return FieldType::kU8;
    case 'h': return FieldType::kI16;
    case 'H': return FieldType::kU16;
    case 'i': case 'l': return FieldType::kI32;
    case 'I': case 'L': return FieldType::kU32;
    case 'q': return FieldType::kI64;
    case 'Q': return FieldType::kU64;
    case 'f': return FieldType::kF32;
    case 'd': return FieldType::kF64;
    default: return FieldType::kNone;
  }
}

RecordLayout oracleGeneralLayout() {
  RecordLayout layout;
  layout.record_size = 24;
  layout.clock_time = {FieldType::kU32, 0, 0, 0};
  layout.obj_id = {FieldType::kU64, 4, 0, 0};
  layout.obj_size = {FieldType::kU32, 12, 0, 0};
  layout.next_access_vtime = {FieldType::kI64, 16, 0, 0};
  return layout;
}

RecordLayout lcsLayout(std::uint64_t version) {
  RecordLayout layout;
  switch (version) {
    case 1:
      // {u32 clock_time, u64 obj_id, u32 obj_size, i64 next_access_vtime}
      layout.record_size = 24;
      layout.clock_time = {FieldType::kU32, 0, 0, 0};
      layout.obj_id = {FieldType::kU64, 4, 0, 0};
      layout.obj_size = {FieldType::kU32, 12, 0, 0};
      layout.next_access_vtime = {FieldType::kI64, 16, 0, 0};
      return layout;
    case 2:
      // v1 plus an op/tenant word: {..., u32 op:8 tenant:24, i64 next}
      layout.record_size = 28;
      layout.clock_time = {FieldType::kU32, 0, 0, 0};
      layout.obj_id = {FieldType::kU64, 4, 0, 0};
      layout.obj_size = {FieldType::kU32, 12, 0, 0};
      layout.op = {FieldType::kU32, 16, 0, 8};
      layout.next_access_vtime = {FieldType::kI64, 20, 0, 0};
      return layout;
    case 3:
    case 4:
    case 5:
    case 6:
    case 7:
    case 8: {
      // v3 widened obj_size to 64 bits and added a ttl word; v4 through v8
      // append 1, 2, 4, 8 and 16 u32 feature fields after that common
      // 36-byte base. We read the base and, where there is at least one
      // feature, take feature[0] as the CPU id — that is the convention the
      // merged multi-CPU traces use.
      layout.clock_time = {FieldType::kU32, 0, 0, 0};
      layout.obj_id = {FieldType::kU64, 4, 0, 0};
      layout.obj_size = {FieldType::kU64, 12, 0, 0};
      layout.op = {FieldType::kU32, 20, 0, 8};
      layout.next_access_vtime = {FieldType::kI64, 28, 0, 0};
      const std::size_t base = 36;
      static constexpr std::size_t kFeatureCount[] = {0, 0, 0, 0, 1, 2, 4, 8, 16};
      const std::size_t features = kFeatureCount[version];
      layout.record_size = base + features * 4;
      if (features > 0) layout.cpu = {FieldType::kU32, static_cast<std::uint16_t>(base), 0, 0};
      return layout;
    }
    default:
      throw ConfigError("unsupported lcs trace version " + std::to_string(version) +
                        "; this build understands versions 1 through 8");
  }
}

RecordLayout vscsiLayout(int version) {
  RecordLayout layout;
  layout.op_encoding = RecordLayout::OpEncoding::kScsiCommand;
  // VSCSI timestamps are microseconds; every other format we read is in
  // seconds, and time-based warm-up compares them.
  layout.time_divisor = 1000000;
  if (version == 1) {
    // {u32 sn, u32 len, u32 nSG, u16 cmd, u16 ver, u64 lbn, u64 ts}
    layout.record_size = 32;
    layout.obj_size = {FieldType::kU32, 4, 0, 0};
    layout.op = {FieldType::kU16, 12, 0, 0};
    layout.obj_id = {FieldType::kU64, 16, 0, 0};
    layout.clock_time = {FieldType::kU64, 24, 0, 0};
    return layout;
  }
  if (version == 2) {
    // {u16 cmd, u16 ver, u32 sn, u32 len, u32 nSG, u64 lbn, u64 ts, u64 rt}
    layout.record_size = 40;
    layout.op = {FieldType::kU16, 0, 0, 0};
    layout.obj_size = {FieldType::kU32, 8, 0, 0};
    layout.obj_id = {FieldType::kU64, 16, 0, 0};
    layout.clock_time = {FieldType::kU64, 24, 0, 0};
    return layout;
  }
  throw ConfigError("unsupported vscsi trace version " + std::to_string(version));
}

int detectVscsiVersion(const std::byte* data, std::size_t size) {
  // The version is the high byte of a uint16 `ver` field: at offset 2 in a
  // 40-byte v2 record, and at offset 14 in a 32-byte v1 record. Checking two
  // consecutive records makes a coincidence much less likely than checking
  // one, which is what libCacheSim does too.
  constexpr std::size_t kProbeRecords = 2;
  const auto versionAt = [&](std::size_t offset) -> int {
    return loadRaw<std::uint16_t>(data + offset) >> 8;
  };
  if (size >= 40 * kProbeRecords) {
    bool all_v2 = true;
    for (std::size_t i = 0; i < kProbeRecords; ++i) {
      if (versionAt(i * 40 + 2) != 2) all_v2 = false;
    }
    if (all_v2) return 2;
  }
  if (size >= 32 * kProbeRecords) {
    bool all_v1 = true;
    for (std::size_t i = 0; i < kProbeRecords; ++i) {
      if (versionAt(i * 32 + 14) != 1) all_v1 = false;
    }
    if (all_v1) return 1;
  }
  return 0;
}

RecordLayout twrBinLayout() {
  RecordLayout layout;
  layout.record_size = 20;
  layout.clock_time = {FieldType::kU32, 0, 0, 0};
  layout.obj_id = {FieldType::kU64, 4, 0, 0};
  // One u32 at offset 12 holds key_size:10 in the high bits and
  // value_size:22 in the low bits.
  layout.key_size = {FieldType::kU32, 12, 22, 10};
  layout.value_size = {FieldType::kU32, 12, 0, 22};
  // One u32 at offset 16 holds op:8 in the high bits and ttl:24 in the low.
  layout.op = {FieldType::kU32, 16, 24, 8};
  return layout;
}

RecordLayout layoutFromFormatSpec(const BinaryFormatSpec& spec) {
  if (spec.format.empty()) {
    throw ConfigError(
        "a binary trace needs a record layout: pass format=<...> in the "
        "trace's reader params, e.g. "
        "\"format=<IQIq,obj-id-col=2,time-col=1,obj-size-col=3\"");
  }
  std::string_view fmt = spec.format;
  if (fmt.front() == '<') {
    fmt.remove_prefix(1);
  } else if (fmt.front() == '>' || fmt.front() == '!' || fmt.front() == '=' ||
             fmt.front() == '@') {
    throw ConfigError("binary trace format '" + spec.format +
                      "' asks for an endianness this reader does not support; only "
                      "little-endian ('<') is supported");
  }
  if (fmt.empty()) throw ConfigError("binary trace format '" + spec.format + "' has no fields");

  // Offsets of each 1-based column, computed by walking the format string.
  std::vector<Field> fields;
  fields.reserve(fmt.size());
  std::size_t offset = 0;
  for (const char c : fmt) {
    const FieldType type = fieldTypeFromFormatChar(c);
    if (type == FieldType::kNone) {
      throw ConfigError(std::string("binary trace format character '") + c +
                        "' is not recognized; use one of bBchHiIlLqQfd");
    }
    fields.push_back(Field{type, static_cast<std::uint16_t>(offset), 0, 0});
    offset += fieldTypeSize(type);
  }

  RecordLayout layout;
  layout.record_size = offset;

  const auto pick = [&](int column, const char* role) -> Field {
    if (column <= 0) return Field{};
    if (static_cast<std::size_t>(column) > fields.size()) {
      throw ConfigError(std::string(role) + " is column " + std::to_string(column) +
                        ", but the format string '" + spec.format + "' has only " +
                        std::to_string(fields.size()) + " fields");
    }
    return fields[static_cast<std::size_t>(column) - 1];
  };

  layout.obj_id = pick(spec.obj_id_col, "obj-id-col");
  if (!layout.obj_id.present()) {
    throw ConfigError("a binary trace needs obj-id-col to say which field holds the object id");
  }
  layout.clock_time = pick(spec.time_col, "time-col");
  layout.obj_size = pick(spec.obj_size_col, "obj-size-col");
  layout.next_access_vtime = pick(spec.next_access_vtime_col, "next-access-vtime-col");
  layout.op = pick(spec.op_col, "op-col");
  layout.cpu = pick(spec.cpu_col, "cpu-col");
  return layout;
}

}  // namespace cachesim::internal
