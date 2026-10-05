// Replays a trace file against one or more (policy, cache size) pairs and
// reports miss ratios and throughput.
//
// This is the measurement harness, and the comparison point against
// libCacheSim: given the same trace file and the same cache sizes, the miss
// ratios must match digit for digit, because both are computing the same
// deterministic function of the trace. A difference is a bug in one of them,
// not a modelling choice.
//
// Usage:
//   bench TRACE --type oracleGeneral --policy LRU --policy LFU
//         --size 1000 --size 5000 [--requests N] [--threads N]
//         [--ignore-obj-size] [--params "k=4"] [--repeat N]
//
// --threads N runs the (policy, size) pairs concurrently, one per worker,
// which is what the sweep runner will do. Each worker gets its own Cache and
// its own cursor over a shared read-only copy of the trace, so there is no
// synchronization during replay at all.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "internal/cache/cache.hpp"
#include "internal/common/error.hpp"
#include "internal/eviction/policy_registry.hpp"
#include "internal/trace/trace_reader.hpp"

using namespace cachesim;

namespace {

struct Options {
  TraceSpec trace;
  std::vector<std::string> policies;
  std::vector<std::uint64_t> sizes;
  std::string params;
  std::size_t threads = 1;
  int repeat = 1;
  bool stream = false;  // replay from the file each time instead of from memory
  bool csv = false;
};

[[noreturn]] void fail(const std::string& message) {
  std::fprintf(stderr, "bench: %s\n", message.c_str());
  std::exit(1);
}

Options parse(int argc, char** argv) {
  if (argc < 2) {
    fail("usage: bench TRACE [--type T] [--policy P]... [--size N]... "
         "[--requests N] [--threads N] [--params S] [--repeat N] "
         "[--ignore-obj-size] [--stream] [--csv]");
  }
  Options options;
  options.trace.path = argv[1];
  for (int i = 2; i < argc; ++i) {
    const std::string flag = argv[i];
    const auto value = [&]() -> std::string {
      if (i + 1 >= argc) fail("flag '" + flag + "' needs a value");
      return argv[++i];
    };
    if (flag == "--type") options.trace.format = parseTraceFormat(value());
    else if (flag == "--policy") options.policies.push_back(value());
    else if (flag == "--size") {
      const auto parsed = parseSize(value());
      if (!parsed) fail("bad --size");
      options.sizes.push_back(*parsed);
    }
    else if (flag == "--requests") options.trace.num_req = std::strtoll(value().c_str(), nullptr, 10);
    else if (flag == "--threads") options.threads = std::strtoul(value().c_str(), nullptr, 10);
    else if (flag == "--params") options.params = value();
    else if (flag == "--repeat") options.repeat = std::atoi(value().c_str());
    else if (flag == "--sample-ratio") options.trace.sample_ratio = std::strtod(value().c_str(), nullptr);
    else if (flag == "--ignore-obj-size") options.trace.ignore_obj_size = true;
    else if (flag == "--stream") options.stream = true;
    else if (flag == "--csv") options.csv = true;
    else fail("unknown flag '" + flag + "'");
  }
  if (options.policies.empty()) options.policies.push_back("LRU");
  if (options.sizes.empty()) options.sizes.push_back(1024);
  if (options.threads == 0) options.threads = 1;
  return options;
}

struct Job {
  std::string policy;
  std::uint64_t size = 0;
};

struct Outcome {
  Stats stats;
  double seconds = 0.0;
  std::uint64_t policy_metadata_bytes = 0;
};

double seconds(std::chrono::steady_clock::time_point start) {
  const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
  return elapsed.count();
}

std::unique_ptr<Cache> buildCache(const Job& job, const std::string& params) {
  PolicyConfig policy_config;
  policy_config.capacity_bytes = job.size;
  // With unit object sizes the byte capacity *is* the object count, which is
  // the exact hint. With real sizes it is an upper bound, which is a safe
  // over-estimate for a hint -- and the arena grows if it is wrong anyway.
  policy_config.entry_hint = static_cast<std::size_t>(job.size);
  policy_config.params = ParamMap(params);

  CacheOptions options;
  options.capacity_bytes = job.size;
  options.entry_hint = static_cast<std::size_t>(job.size);
  return std::make_unique<Cache>(options,
                                 PolicyRegistry::instance().create(job.policy, policy_config));
}

// Replays an in-memory trace. This is the measurement that isolates the
// simulator's own cost: the trace is already decoded, so what is being timed
// is the hash lookup, the policy, and the eviction work.
Outcome replayMemory(const Job& job, const std::vector<Request>& trace,
                     const std::string& params) {
  std::unique_ptr<Cache> cache = buildCache(job, params);
  const auto start = std::chrono::steady_clock::now();
  for (const Request& req : trace) cache->access(req);
  Outcome outcome;
  outcome.seconds = seconds(start);
  outcome.stats = cache->stats();
  outcome.policy_metadata_bytes = cache->policy().metadataBytes();
  return outcome;
}

// Replays straight from the file, which is what a sweep over a trace too
// large to hold in memory does. Includes decode cost.
Outcome replayStream(const Job& job, const TraceSpec& spec, const std::string& params) {
  std::unique_ptr<Cache> cache = buildCache(job, params);
  std::unique_ptr<ITraceReader> reader = openTrace(spec);
  constexpr std::size_t kBatch = 1u << 14;
  std::vector<Request> batch(kBatch);
  const auto start = std::chrono::steady_clock::now();
  for (;;) {
    const std::size_t got = reader->nextBatch(batch.data(), kBatch);
    if (got == 0) break;
    for (std::size_t i = 0; i < got; ++i) cache->access(batch[i]);
  }
  Outcome outcome;
  outcome.seconds = seconds(start);
  outcome.stats = cache->stats();
  outcome.policy_metadata_bytes = cache->policy().metadataBytes();
  return outcome;
}

}  // namespace

int main(int argc, char** argv) {
  const Options options = parse(argc, argv);

  std::vector<Job> jobs;
  for (const std::string& policy : options.policies) {
    for (const std::uint64_t size : options.sizes) jobs.push_back(Job{policy, size});
  }

  try {
    std::vector<Request> trace;
    if (!options.stream) {
      std::fprintf(stderr, "bench: loading %s ...\n", options.trace.path.c_str());
      const auto load_start = std::chrono::steady_clock::now();
      trace = materializeTrace(options.trace);
      const double load_seconds = seconds(load_start);
      std::fprintf(stderr,
                   "bench: %zu requests in %.2f s (%.1f M req/s decode, %.2f GiB resident)\n",
                   trace.size(), load_seconds,
                   static_cast<double>(trace.size()) / load_seconds / 1e6,
                   static_cast<double>(trace.size() * sizeof(Request)) / (1024.0 * 1024 * 1024));
      if (trace.empty()) fail("trace is empty");
    }

    std::vector<Outcome> outcomes(jobs.size());

    for (int round = 0; round < options.repeat; ++round) {
      const auto wall_start = std::chrono::steady_clock::now();
      if (options.threads <= 1) {
        for (std::size_t i = 0; i < jobs.size(); ++i) {
          outcomes[i] = options.stream ? replayStream(jobs[i], options.trace, options.params)
                                       : replayMemory(jobs[i], trace, options.params);
        }
      } else {
        // One atomic counter is the entire work distribution: each worker
        // takes the next job and runs it to completion. No shared mutable
        // state during replay, so no locks and no barriers.
        std::atomic<std::size_t> next{0};
        std::vector<std::thread> workers;
        const std::size_t worker_count = std::min(options.threads, jobs.size());
        workers.reserve(worker_count);
        for (std::size_t w = 0; w < worker_count; ++w) {
          workers.emplace_back([&] {
            for (;;) {
              const std::size_t index = next.fetch_add(1, std::memory_order_relaxed);
              if (index >= jobs.size()) return;
              outcomes[index] = options.stream
                                    ? replayStream(jobs[index], options.trace, options.params)
                                    : replayMemory(jobs[index], trace, options.params);
            }
          });
        }
        for (std::thread& worker : workers) worker.join();
      }
      const double wall = seconds(wall_start);

      if (round + 1 < options.repeat) {
        std::fprintf(stderr, "bench: round %d/%d warm-up, %.2f s\n", round + 1, options.repeat,
                     wall);
        continue;
      }

      if (options.csv) {
        std::printf("policy,cache_size,n_req,n_miss,miss_ratio,byte_miss_ratio,seconds,mrps\n");
      } else {
        std::printf("\n%-12s %12s %12s %10s %10s %9s %9s\n", "policy", "cache_size", "n_req",
                    "miss_ratio", "byte_mr", "seconds", "M req/s");
        std::printf("%s\n", std::string(80, '-').c_str());
      }
      double total_cpu = 0.0;
      for (std::size_t i = 0; i < jobs.size(); ++i) {
        const Outcome& outcome = outcomes[i];
        const double mrps =
            static_cast<double>(outcome.stats.n_req) / outcome.seconds / 1e6;
        total_cpu += outcome.seconds;
        if (options.csv) {
          std::printf("%s,%llu,%llu,%llu,%.6f,%.6f,%.4f,%.2f\n", jobs[i].policy.c_str(),
                      static_cast<unsigned long long>(jobs[i].size),
                      static_cast<unsigned long long>(outcome.stats.n_req),
                      static_cast<unsigned long long>(outcome.stats.nMiss()),
                      outcome.stats.missRatio(), outcome.stats.byteMissRatio(),
                      outcome.seconds, mrps);
        } else {
          std::printf("%-12s %12llu %12llu %10.6f %10.6f %9.3f %9.2f\n", jobs[i].policy.c_str(),
                      static_cast<unsigned long long>(jobs[i].size),
                      static_cast<unsigned long long>(outcome.stats.n_req),
                      outcome.stats.missRatio(), outcome.stats.byteMissRatio(),
                      outcome.seconds, mrps);
        }
      }
      if (!options.csv) {
        std::printf("\n%zu jobs, %.2f s wall, %.2f s summed CPU, speedup %.2fx on %zu threads\n",
                    jobs.size(), wall, total_cpu, total_cpu / wall, options.threads);
      }
    }
  } catch (const Error& error) {
    fail(error.what());
  }
  return 0;
}
