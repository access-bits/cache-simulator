#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "internal/common/compiler.hpp"
#include "internal/request/request.hpp"

namespace cachesim::internal {

// Width and signedness of one field inside a fixed-size binary record.
enum class FieldType : std::uint8_t {
  kNone,
  kI8, kU8,
  kI16, kU16,
  kI32, kU32,
  kI64, kU64,
  kF32, kF64,
};

[[nodiscard]] std::size_t fieldTypeSize(FieldType type);

// Maps one Python-struct format character to a field type, as libCacheSim's
// binary reader does: b/B/c are 1 byte, h/H 2, i/I/l/L 4, q/Q 8, f 4, d 8,
// with lowercase signed and uppercase unsigned.
//
// (libCacheSim reads every one of these as *signed*, which sign-extends an
// unsigned 32-bit field whose top bit is set. We read them as declared. For
// an object id either reading is injective so hit ratios are unaffected, but
// for a size field the signed reading produces a negative number, so this is
// a deliberate and, I think, uncontroversial deviation.)
[[nodiscard]] FieldType fieldTypeFromFormatChar(char c);

// One field: where it is, how wide, and optionally which bits of it to use.
//
// The bit range exists because real trace formats pack several values into
// one integer — Twitter's binary format puts a 10-bit key size and a 22-bit
// value size in one uint32, and libCacheSim's lcs v2 packs an 8-bit op and a
// 24-bit tenant id together.
struct Field {
  FieldType type = FieldType::kNone;
  std::uint16_t offset = 0;
  std::uint8_t bit_shift = 0;
  std::uint8_t bit_width = 0;  // 0 means "the whole field"

  [[nodiscard]] bool present() const { return type != FieldType::kNone; }
};

// A fixed-size record layout: which bytes of each record hold what.
//
// One table-driven decoder serves every fixed-record format — oracleGeneral,
// lcs v1 through v8, Twitter binary, VSCSI v1 and v2, and whatever a user
// describes with a format string — rather than a hand-written reader per
// format. That is roughly 400 lines of near-duplicate C in libCacheSim
// replaced by a struct and one loop, and it means a new fixed-record format
// is a table entry rather than a new file.
struct RecordLayout {
  std::size_t record_size = 0;

  Field obj_id;
  Field clock_time;
  Field obj_size;
  Field next_access_vtime;
  Field op;
  Field cpu;
  // Key and value sizes, for key-value formats that store them separately;
  // when both are present the object size is their sum.
  Field key_size;
  Field value_size;

  // Post-processing applied to the object id, in this order:
  //   obj_id >>= page_shift     (a memory trace of byte addresses -> pages)
  //   obj_id /= block_size      (a block trace of byte offsets -> blocks)
  std::uint8_t page_shift = 0;
  std::uint32_t block_size = 0;

  // Divides the raw timestamp. VSCSI traces store microseconds where every
  // other format stores seconds, and warm-up by trace time has to compare
  // like with like.
  std::uint32_t time_divisor = 1;

  // How to turn the format's operation field into an Op. SCSI command codes
  // are not Op values, so they need a mapping table rather than a cast.
  enum class OpEncoding : std::uint8_t { kDirect, kScsiCommand } op_encoding = OpEncoding::kDirect;

  // Size to use when the layout has no size field at all (an id-only trace).
  // 1 makes capacity an object count, which is what such traces mean.
  std::uint32_t default_size = 1;

  // Map the format's "no next access" sentinel onto kNeverAgain. libCacheSim
  // writes -1 for it in some formats and INT64_MAX in others, and both must
  // end up sorting above every real time.
  bool normalize_oracle_sentinel = true;

  [[nodiscard]] bool valid() const { return record_size > 0 && obj_id.present(); }
};

// SCSI command codes, as they appear in VSCSI block traces. The lists are
// libCacheSim's, which in turn come from the trace tooling that produced
// those files: 8/40/45/136/168 are the READ variants and
// 42/63/138/142/154/156/170/174 the WRITE variants.
[[nodiscard]] CACHESIM_ALWAYS_INLINE Op opFromScsiCommand(std::uint16_t cmd) {
  switch (cmd) {
    case 8: case 40: case 45: case 136: case 168:
      return Op::kRead;
    case 42: case 63: case 138: case 142: case 154: case 156: case 170: case 174:
      return Op::kWrite;
    default:
      return Op::kInvalid;
  }
}

// Loads a field as a 64-bit value, applying its bit range. memcpy rather than
// a cast because trace records are packed and therefore frequently
// misaligned; memcpy of a constant size compiles to a single load, and
// unlike the cast it is not undefined behaviour.
template <typename T>
[[nodiscard]] CACHESIM_ALWAYS_INLINE T loadRaw(const std::byte* p) {
  T value;
  std::memcpy(&value, p, sizeof(T));
  return value;
}

[[nodiscard]] CACHESIM_ALWAYS_INLINE std::uint64_t loadField(const std::byte* record,
                                                             const Field& field) {
  std::uint64_t value = 0;
  switch (field.type) {
    case FieldType::kNone:
      return 0;
    case FieldType::kI8:
      value = static_cast<std::uint64_t>(static_cast<std::int64_t>(loadRaw<std::int8_t>(record + field.offset)));
      break;
    case FieldType::kU8:
      value = loadRaw<std::uint8_t>(record + field.offset);
      break;
    case FieldType::kI16:
      value = static_cast<std::uint64_t>(static_cast<std::int64_t>(loadRaw<std::int16_t>(record + field.offset)));
      break;
    case FieldType::kU16:
      value = loadRaw<std::uint16_t>(record + field.offset);
      break;
    case FieldType::kI32:
      value = static_cast<std::uint64_t>(static_cast<std::int64_t>(loadRaw<std::int32_t>(record + field.offset)));
      break;
    case FieldType::kU32:
      value = loadRaw<std::uint32_t>(record + field.offset);
      break;
    case FieldType::kI64:
      value = static_cast<std::uint64_t>(loadRaw<std::int64_t>(record + field.offset));
      break;
    case FieldType::kU64:
      value = loadRaw<std::uint64_t>(record + field.offset);
      break;
    case FieldType::kF32:
      value = static_cast<std::uint64_t>(static_cast<std::int64_t>(loadRaw<float>(record + field.offset)));
      break;
    case FieldType::kF64:
      value = static_cast<std::uint64_t>(static_cast<std::int64_t>(loadRaw<double>(record + field.offset)));
      break;
  }
  if (field.bit_width != 0) {
    const std::uint64_t mask =
        field.bit_width >= 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << field.bit_width) - 1);
    value = (value >> field.bit_shift) & mask;
  }
  return value;
}

// Decodes one record into a Request. The branches are on a layout that is
// fixed for the whole run, so they are perfectly predicted after the first
// few records; what matters is that there is no allocation, no copy of the
// record, and no call out of line.
CACHESIM_ALWAYS_INLINE void decodeRecord(const std::byte* record, const RecordLayout& layout,
                                         Request& out) {
  std::uint64_t id = loadField(record, layout.obj_id);
  if (layout.page_shift != 0) id >>= layout.page_shift;
  if (layout.block_size != 0) id /= layout.block_size;
  out.obj_id = id;

  if (layout.clock_time.present()) {
    auto time = static_cast<std::int64_t>(loadField(record, layout.clock_time));
    if (layout.time_divisor > 1) time /= layout.time_divisor;
    out.clock_time = time;
  } else {
    out.clock_time = 0;
  }

  if (layout.obj_size.present()) {
    out.size = static_cast<std::uint32_t>(loadField(record, layout.obj_size));
  } else if (layout.key_size.present() || layout.value_size.present()) {
    out.size = static_cast<std::uint32_t>(loadField(record, layout.key_size) +
                                          loadField(record, layout.value_size));
  } else {
    out.size = layout.default_size;
  }

  if (layout.next_access_vtime.present()) {
    auto next = static_cast<std::int64_t>(loadField(record, layout.next_access_vtime));
    if (layout.normalize_oracle_sentinel && (next == -1 || next == kNeverAgain)) {
      next = kNeverAgain;
    }
    out.next_access_vtime = next;
  } else {
    out.next_access_vtime = kNoOracle;
  }

  if (!layout.op.present()) {
    out.op = Op::kUnknown;
  } else {
    const std::uint64_t raw_op = loadField(record, layout.op);
    out.op = layout.op_encoding == RecordLayout::OpEncoding::kScsiCommand
                 ? opFromScsiCommand(static_cast<std::uint16_t>(raw_op))
                 : static_cast<Op>(raw_op);
  }
  out.cpu_id =
      layout.cpu.present() ? static_cast<std::uint16_t>(loadField(record, layout.cpu)) : 0;
  out.flags = 0;
}

// ---------------------------------------------------------------- presets

// libCacheSim's oracleGeneral: {u32 clock_time, u64 obj_id, u32 obj_size,
// i64 next_access_vtime}, packed, 24 bytes.
[[nodiscard]] RecordLayout oracleGeneralLayout();

// libCacheSim's lcs record formats, selected by the version in the file
// header. Throws ConfigError for an unsupported version.
[[nodiscard]] RecordLayout lcsLayout(std::uint64_t version);

// VSCSI block traces. Version 1 records are 32 bytes, version 2 records are
// 40; the version lives in the high byte of a `ver` field whose position
// differs between the two, which is what detectVscsiVersion sorts out.
[[nodiscard]] RecordLayout vscsiLayout(int version);

// Reads the first records of a mapped VSCSI file and returns 1, 2, or 0 if
// neither layout's version byte is consistent.
[[nodiscard]] int detectVscsiVersion(const std::byte* data, std::size_t size);

// Twitter key-value binary: {u32 real_time, u64 obj_id, u32 kv_size,
// u32 op_ttl}, packed, 20 bytes, with key/value sizes and op/ttl bit-packed.
[[nodiscard]] RecordLayout twrBinLayout();

// Builds a layout from a Python-struct-style format string plus 1-based
// column indices, matching libCacheSim's `format=<...>` reader parameter.
// `format` must start with '<' (little-endian; nothing else is supported,
// and nothing else occurs in practice).
struct BinaryFormatSpec {
  std::string format;  // e.g. "<IQIq"
  int obj_id_col = 0;  // 1-based; required
  int time_col = 0;
  int obj_size_col = 0;
  int next_access_vtime_col = 0;
  int op_col = 0;
  int cpu_col = 0;
};
[[nodiscard]] RecordLayout layoutFromFormatSpec(const BinaryFormatSpec& spec);

}  // namespace cachesim::internal
