// Generates a synthetic trace in libCacheSim's oracleGeneral binary format,
// so that this simulator and libCacheSim can be run against byte-identical
// input and their results compared exactly.
//
//   oracleGeneral record, 24 bytes, packed, little-endian:
//     uint32 clock_time
//     uint64 obj_id
//     uint32 obj_size
//     int64  next_access_vtime   (-1 if never requested again)
//
// Usage:
//   gen_trace out.oracleGeneral --requests 1000000000 --objects 100000000
//             [--zipf 1.0] [--seed 42] [--size-min 1 --size-max 1]
//
// The oracle column is the hard part. next_access_vtime is the *logical index
// of the next request for this object*, which is only known once the rest of
// the trace exists. Computing it needs either the whole trace in memory (24 GB
// for a billion requests) or two passes over a file. This does two passes:
//
//   pass 1  generate ids and sizes, writing records with a placeholder
//           oracle column, and remember nothing but the RNG seed
//   pass 2  walk the file *backwards* in chunks, keeping a map of
//           obj_id -> the index where it is next seen, and patch each
//           record's oracle column in place
//
// Pass 2's map is bounded by the number of distinct objects, not by the
// number of requests, which is what makes a billion-request trace tractable
// in 15 GB of RAM.

#include <sys/types.h>

#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "ankerl/unordered_dense.h"
#include "internal/common/hash.hpp"

using cachesim::Rng;

namespace {

constexpr std::size_t kRecordSize = 24;

#pragma pack(push, 1)
struct OracleRecord {
  std::uint32_t clock_time;
  std::uint64_t obj_id;
  std::uint32_t obj_size;
  std::int64_t next_access_vtime;
};
#pragma pack(pop)
static_assert(sizeof(OracleRecord) == kRecordSize);

[[noreturn]] void fail(const std::string& message) {
  std::fprintf(stderr, "gen_trace: %s\n", message.c_str());
  std::exit(1);
}

struct Options {
  std::string path;
  std::uint64_t requests = 1'000'000;
  std::uint64_t objects = 100'000;
  double zipf = 1.0;
  std::uint64_t seed = 42;
  std::uint32_t size_min = 1;
  std::uint32_t size_max = 1;
  // Requests per second of simulated wall-clock time, for the timestamp
  // column. Only time-based warm-up reads it.
  std::uint64_t rate = 100'000;
};

Options parse(int argc, char** argv) {
  if (argc < 2) {
    fail("usage: gen_trace OUT.oracleGeneral --requests N --objects N "
         "[--zipf A] [--seed N] [--size-min N] [--size-max N]");
  }
  Options options;
  options.path = argv[1];
  for (int i = 2; i + 1 < argc; i += 2) {
    const std::string flag = argv[i];
    const std::string value = argv[i + 1];
    if (flag == "--requests") options.requests = std::strtoull(value.c_str(), nullptr, 10);
    else if (flag == "--objects") options.objects = std::strtoull(value.c_str(), nullptr, 10);
    else if (flag == "--zipf") options.zipf = std::strtod(value.c_str(), nullptr);
    else if (flag == "--seed") options.seed = std::strtoull(value.c_str(), nullptr, 10);
    else if (flag == "--size-min") options.size_min = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
    else if (flag == "--size-max") options.size_max = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
    else if (flag == "--rate") options.rate = std::strtoull(value.c_str(), nullptr, 10);
    else fail("unknown flag '" + flag + "'");
  }
  if (options.objects == 0) fail("--objects must be positive");
  if (options.size_max < options.size_min) fail("--size-max must be >= --size-min");
  return options;
}

// Zipf sampling without a CDF table.
//
// A CDF over 100 million objects is 800 MB and a binary search through it is a
// guaranteed cache miss per draw, which would make trace generation slower
// than the simulation it feeds. Instead this inverts the continuous
// approximation of the Zipf CDF, which for exponent a != 1 is
//
//     rank(u) = ((1 - u) * 1^(1-a) + u * N^(1-a)) ^ (1/(1-a))
//
// and for a == 1 reduces to N^u. Two transcendental calls per draw, no
// memory traffic, and the rank distribution matches a true Zipf closely
// enough for a cache benchmark (the discretization error is concentrated in
// the few most popular ranks, where the counts are enormous either way).
class ZipfSampler {
 public:
  ZipfSampler(std::uint64_t objects, double exponent)
      : objects_(objects), exponent_(exponent) {
    if (std::fabs(exponent_ - 1.0) < 1e-9) {
      unit_exponent_ = true;
      log_n_ = std::log(static_cast<double>(objects_));
    } else {
      const double one_minus_a = 1.0 - exponent_;
      n_pow_ = std::pow(static_cast<double>(objects_), one_minus_a);
      inv_one_minus_a_ = 1.0 / one_minus_a;
    }
  }

  std::uint64_t operator()(Rng& rng) const {
    const double u = rng.nextDouble();
    double rank;
    if (unit_exponent_) {
      rank = std::exp(u * log_n_);
    } else {
      rank = std::pow(1.0 + u * (n_pow_ - 1.0), inv_one_minus_a_);
    }
    auto id = static_cast<std::uint64_t>(rank);
    if (id >= objects_) id = objects_ - 1;
    return id;
  }

 private:
  std::uint64_t objects_;
  double exponent_;
  bool unit_exponent_ = false;
  double log_n_ = 0.0;
  double n_pow_ = 0.0;
  double inv_one_minus_a_ = 0.0;
};

void writeAll(std::FILE* file, const void* data, std::size_t bytes, const std::string& path) {
  if (std::fwrite(data, 1, bytes, file) != bytes) {
    fail("short write to '" + path + "': " + std::strerror(errno));
  }
}

void readAt(std::FILE* file, std::uint64_t offset, void* data, std::size_t bytes,
            const std::string& path) {
  if (::fseeko(file, static_cast<off_t>(offset), SEEK_SET) != 0) {
    fail("seek failed in '" + path + "': " + std::strerror(errno));
  }
  if (std::fread(data, 1, bytes, file) != bytes) {
    fail("short read from '" + path + "': " + std::strerror(errno));
  }
}

void writeAt(std::FILE* file, std::uint64_t offset, const void* data, std::size_t bytes,
             const std::string& path) {
  if (::fseeko(file, static_cast<off_t>(offset), SEEK_SET) != 0) {
    fail("seek failed in '" + path + "': " + std::strerror(errno));
  }
  writeAll(file, data, bytes, path);
}

double seconds(std::chrono::steady_clock::time_point start) {
  const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
  return elapsed.count();
}

}  // namespace

int main(int argc, char** argv) {
  const Options options = parse(argc, argv);
  const double gigabytes =
      static_cast<double>(options.requests) * kRecordSize / (1024.0 * 1024.0 * 1024.0);

  std::printf("gen_trace: %llu requests over %llu objects, Zipf(%.2f), seed %llu\n",
              static_cast<unsigned long long>(options.requests),
              static_cast<unsigned long long>(options.objects), options.zipf,
              static_cast<unsigned long long>(options.seed));
  std::printf("           sizes %u..%u, output %s (%.2f GiB)\n", options.size_min,
              options.size_max, options.path.c_str(), gigabytes);

  // ---------------------------------------------------------------- pass 1
  {
    std::FILE* out = std::fopen(options.path.c_str(), "wb");
    if (out == nullptr) fail("cannot create '" + options.path + "': " + std::strerror(errno));

    constexpr std::size_t kBatch = 1u << 16;
    std::vector<OracleRecord> batch(kBatch);
    Rng rng(options.seed);
    const ZipfSampler sample(options.objects, options.zipf);
    const std::uint32_t size_span = options.size_max - options.size_min + 1;

    const auto start = std::chrono::steady_clock::now();
    std::uint64_t written = 0;
    while (written < options.requests) {
      const std::size_t n =
          static_cast<std::size_t>(std::min<std::uint64_t>(kBatch, options.requests - written));
      for (std::size_t i = 0; i < n; ++i) {
        OracleRecord& record = batch[i];
        record.obj_id = sample(rng);
        record.obj_size =
            size_span == 1 ? options.size_min
                           : options.size_min + static_cast<std::uint32_t>(rng.below(size_span));
        record.clock_time = static_cast<std::uint32_t>((written + i) / options.rate);
        record.next_access_vtime = -1;  // patched in pass 2
      }
      writeAll(out, batch.data(), n * kRecordSize, options.path);
      written += n;
      if (written % (1ULL << 26) == 0) {
        std::printf("  pass 1: %llu / %llu (%.1f%%), %.1f M rec/s\n",
                    static_cast<unsigned long long>(written),
                    static_cast<unsigned long long>(options.requests),
                    100.0 * static_cast<double>(written) / static_cast<double>(options.requests),
                    static_cast<double>(written) / seconds(start) / 1e6);
        std::fflush(stdout);
      }
    }
    if (std::fclose(out) != 0) fail("closing '" + options.path + "' failed");
    std::printf("  pass 1 done in %.1f s\n", seconds(start));
  }

  // ---------------------------------------------------------------- pass 2
  //
  // Walk backwards, patching each record's oracle column. The map holds one
  // entry per *distinct object*, not per request, so its size is bounded by
  // --objects: 100 M objects at 16 bytes per entry is 1.6 GB, which fits
  // where a billion-request in-memory trace would not.
  {
    std::FILE* file = std::fopen(options.path.c_str(), "r+b");
    if (file == nullptr) fail("cannot reopen '" + options.path + "': " + std::strerror(errno));

    ankerl::unordered_dense::map<std::uint64_t, std::int64_t> next_index;
    next_index.reserve(std::min<std::uint64_t>(options.objects, 200'000'000));

    constexpr std::size_t kChunk = 1u << 20;  // 1 M records = 24 MiB
    std::vector<OracleRecord> chunk(kChunk);

    const auto start = std::chrono::steady_clock::now();
    std::uint64_t remaining = options.requests;
    while (remaining > 0) {
      const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(kChunk, remaining));
      const std::uint64_t first = remaining - n;
      const std::uint64_t offset = first * kRecordSize;
      readAt(file, offset, chunk.data(), n * kRecordSize, options.path);

      // Backwards within the chunk, so that "next" is always already known.
      for (std::size_t i = n; i-- > 0;) {
        const std::uint64_t index = first + i;
        const auto it = next_index.find(chunk[i].obj_id);
        chunk[i].next_access_vtime = it == next_index.end() ? -1 : it->second;
        next_index[chunk[i].obj_id] = static_cast<std::int64_t>(index);
      }

      writeAt(file, offset, chunk.data(), n * kRecordSize, options.path);
      remaining = first;

      const std::uint64_t done = options.requests - remaining;
      if (done % (1ULL << 26) == 0) {
        std::printf("  pass 2: %llu / %llu (%.1f%%), %.1f M rec/s, %zu distinct so far\n",
                    static_cast<unsigned long long>(done),
                    static_cast<unsigned long long>(options.requests),
                    100.0 * static_cast<double>(done) / static_cast<double>(options.requests),
                    static_cast<double>(done) / seconds(start) / 1e6, next_index.size());
        std::fflush(stdout);
      }
    }
    std::printf("  pass 2 done in %.1f s, %zu distinct objects\n", seconds(start),
                next_index.size());
    if (std::fclose(file) != 0) fail("closing '" + options.path + "' failed");
  }

  std::printf("wrote %s\n", options.path.c_str());
  return 0;
}
