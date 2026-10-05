// A complete, runnable example of driving the simulator as a library.
//
// It builds a synthetic Zipf-like trace in memory, replays it against every
// registered policy at several capacities, and prints a miss-ratio table with
// Belady's offline optimum alongside for reference.
//
// Build and run:
//   cmake -S . -B build && cmake --build build -j
//   ./build/examples/replay
//   ./build/examples/replay --requests 2000000 --objects 200000
//
// This exists because it is the shortest honest answer to "how do I use
// this": six lines to construct a cache, one to replay a request, one to read
// the result.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "internal/cache/cache.hpp"
#include "internal/common/hash.hpp"
#include "internal/eviction/policy_registry.hpp"

using namespace cachesim;

namespace {

// Zipf-like popularity by inverse-transform sampling on a uniform draw.
// Exponent 1.0 (the classic Zipf) is close to what web and key-value caches
// actually see; a real study would read a real trace, which is what the trace
// readers are for.
std::vector<Request> makeZipfTrace(std::size_t requests, std::uint64_t objects,
                                   double exponent, std::uint64_t seed) {
  // Precompute the CDF so each draw is one binary search rather than a sum.
  std::vector<double> cdf(objects);
  double total = 0.0;
  for (std::uint64_t i = 0; i < objects; ++i) {
    total += 1.0 / std::pow(static_cast<double>(i + 1), exponent);
    cdf[i] = total;
  }
  for (double& value : cdf) value /= total;

  Rng rng(seed);
  std::vector<Request> trace;
  trace.reserve(requests);
  for (std::size_t i = 0; i < requests; ++i) {
    const double u = rng.nextDouble();
    const auto it = std::lower_bound(cdf.begin(), cdf.end(), u);
    Request req;
    req.obj_id = static_cast<std::uint64_t>(std::distance(cdf.begin(), it));
    req.size = 1;  // object-count capacity; see the note on sizes below
    req.clock_time = static_cast<std::int64_t>(i);
    trace.push_back(req);
  }
  return trace;
}

// Belady needs to know, for each request, the index at which that object is
// next requested. A real oracle trace carries this as a column; here we
// compute it by walking the trace backwards once.
void fillOracleColumn(std::vector<Request>& trace) {
  std::unordered_map<std::uint64_t, std::size_t> next_seen;
  next_seen.reserve(trace.size() / 4);
  for (std::size_t i = trace.size(); i-- > 0;) {
    const auto it = next_seen.find(trace[i].obj_id);
    trace[i].next_access_vtime =
        it == next_seen.end() ? kNeverAgain : static_cast<std::int64_t>(it->second);
    next_seen[trace[i].obj_id] = i;
  }
}

struct Result {
  double miss_ratio = 0.0;
  double seconds = 0.0;
  double million_requests_per_second = 0.0;
};

Result replay(const std::string& policy_name, const std::vector<Request>& trace,
              std::uint64_t capacity) {
  // 1. Describe the policy. Capacity and the expected object count are given
  //    at construction because most policies need them to size themselves
  //    (ARC's ghost lists, S3FIFO's 10/90 split, every queue's node pool).
  PolicyConfig policy_config;
  policy_config.capacity_bytes = capacity;
  policy_config.entry_hint = static_cast<std::size_t>(capacity);
  policy_config.params = ParamMap("");  // e.g. ParamMap("k=4") for LRU-K

  // 2. Describe the cache.
  CacheOptions options;
  options.capacity_bytes = capacity;
  options.entry_hint = static_cast<std::size_t>(capacity);

  // 3. Build it. This is the one place a runtime string becomes a concrete
  //    policy; everything after it is monomorphic.
  Cache cache(options, PolicyRegistry::instance().create(policy_name, policy_config));

  // 4. Replay.
  const auto start = std::chrono::steady_clock::now();
  for (const Request& req : trace) {
    cache.access(req);
  }
  const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;

  // 5. Read the result.
  Result result;
  result.miss_ratio = cache.stats().missRatio();
  result.seconds = elapsed.count();
  result.million_requests_per_second =
      static_cast<double>(cache.stats().n_req) / elapsed.count() / 1e6;
  return result;
}

std::int64_t argInt(int argc, char** argv, const char* flag, std::int64_t fallback) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], flag) == 0) return std::strtoll(argv[i + 1], nullptr, 10);
  }
  return fallback;
}

}  // namespace

int main(int argc, char** argv) {
  const auto requests = static_cast<std::size_t>(argInt(argc, argv, "--requests", 1'000'000));
  const auto objects = static_cast<std::uint64_t>(argInt(argc, argv, "--objects", 100'000));
  const auto seed = static_cast<std::uint64_t>(argInt(argc, argv, "--seed", 42));

  std::printf("Building a Zipf(1.0) trace: %zu requests over %llu objects\n", requests,
              static_cast<unsigned long long>(objects));
  std::vector<Request> trace = makeZipfTrace(requests, objects, 1.0, seed);
  fillOracleColumn(trace);

  // Capacities as a fraction of the object universe, which is how miss-ratio
  // curves are normally read.
  const std::vector<double> fractions{0.001, 0.01, 0.05, 0.10};
  std::vector<std::uint64_t> capacities;
  for (const double fraction : fractions) {
    capacities.push_back(std::max<std::uint64_t>(
        1, static_cast<std::uint64_t>(static_cast<double>(objects) * fraction)));
  }

  std::printf("\nMiss ratio by cache size (objects). Lower is better; Belady is the\n");
  std::printf("offline optimum, so no online policy can beat it.\n\n");
  std::printf("%-12s", "policy");
  for (std::size_t i = 0; i < capacities.size(); ++i) {
    char header[32];
    std::snprintf(header, sizeof(header), "%.1f%%", fractions[i] * 100);
    std::printf("%12s", header);
  }
  std::printf("%14s\n", "M req/s");
  std::printf("%s\n", std::string(12 + 12 * capacities.size() + 14, '-').c_str());

  for (const std::string& policy : PolicyRegistry::instance().names()) {
    std::printf("%-12s", policy.c_str());
    double throughput_sum = 0.0;
    for (const std::uint64_t capacity : capacities) {
      const Result result = replay(policy, trace, capacity);
      std::printf("%12.4f", result.miss_ratio);
      throughput_sum += result.million_requests_per_second;
    }
    std::printf("%14.1f\n", throughput_sum / static_cast<double>(capacities.size()));
  }

  std::printf(
      "\nNote on sizes: this example gives every object size 1, so capacity is an\n"
      "object count. The engine is byte-based throughout -- give requests real\n"
      "sizes and capacity means bytes, and the byte miss ratio becomes meaningful.\n");
  return 0;
}
