// Compares the ways a single trace can be delivered to many concurrent
// simulations.
//
// This is the measurement that decides how a parameter sweep scales. Each
// configuration is an independent cache sharing nothing with the others, so
// the trace is the only coupling left -- and past a handful of
// configurations, how that coupling is arranged matters more than anything
// inside the cache engine.
//
// Usage:
//   replay_bench TRACE --policy LRU --size 100000 --size 1000000
//                [--strategy shared-ring] [--requests N] [--ring-mb N]
//                [--stride N] [--batch N] [--repeat N] [--all]
//
// --all runs every strategy in turn and checks that they agree: the miss
// counts must be identical, because the trace delivered is the same trace.
// A difference is a delivery bug, which is exactly the kind of thing that
// otherwise shows up as two sweep rows that quietly are not comparable.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

#include "internal/cache/cache.hpp"
#include "internal/common/error.hpp"
#include "internal/eviction/policy_registry.hpp"
#include "internal/sim/replay.hpp"

using namespace cachesim;

namespace {

struct Options {
  TraceSpec trace;
  std::vector<std::string> policies;
  std::vector<std::uint64_t> sizes;
  std::string params;
  ReplayOptions replay;
  bool all_strategies = false;
  int repeat = 1;
};

[[noreturn]] void fail(const std::string& message) {
  std::fprintf(stderr, "replay_bench: %s\n", message.c_str());
  std::exit(1);
}

Options parse(int argc, char** argv) {
  if (argc < 2) {
    fail("usage: replay_bench TRACE [--type T] [--policy P]... [--size N]... "
         "[--strategy S] [--requests N] [--ring-mb N] [--stride N] [--batch N] "
         "[--repeat N] [--all]");
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
    } else if (flag == "--requests") {
      options.trace.num_req = std::strtoll(value().c_str(), nullptr, 10);
    } else if (flag == "--params") options.params = value();
    else if (flag == "--strategy") options.replay.strategy = parseReplayStrategy(value());
    else if (flag == "--ring-mb") {
      options.replay.ring_bytes = std::strtoull(value().c_str(), nullptr, 10) * 1024ULL * 1024;
    } else if (flag == "--stride") {
      options.replay.publish_stride = std::strtoull(value().c_str(), nullptr, 10);
    } else if (flag == "--batch") {
      options.replay.batch_requests = std::strtoull(value().c_str(), nullptr, 10);
    } else if (flag == "--read-batch") {
      options.replay.read_batch = std::strtoull(value().c_str(), nullptr, 10);
    } else if (flag == "--repeat") options.repeat = std::atoi(value().c_str());
    else if (flag == "--ignore-obj-size") options.trace.ignore_obj_size = true;
    else if (flag == "--all") options.all_strategies = true;
    else if (flag == "--no-auto-materialize") {
      options.replay.prefer_materialized_when_it_fits = false;
    } else fail("unknown flag '" + flag + "'");
  }
  if (options.policies.empty()) options.policies.push_back("LRU");
  if (options.sizes.empty()) options.sizes.push_back(1000000);
  return options;
}

struct Job {
  std::string policy;
  std::uint64_t size = 0;
};

std::unique_ptr<Cache> buildCache(const Job& job, const std::string& params) {
  PolicyConfig policy_config;
  policy_config.capacity_bytes = job.size;
  policy_config.entry_hint = static_cast<std::size_t>(job.size);
  policy_config.params = ParamMap(params);
  CacheOptions cache_options;
  cache_options.capacity_bytes = job.size;
  cache_options.entry_hint = static_cast<std::size_t>(job.size);
  return std::make_unique<Cache>(cache_options,
                                 PolicyRegistry::instance().create(job.policy, policy_config));
}

struct Outcome {
  ReplayStats stats;
  std::vector<std::uint64_t> misses;
  std::vector<std::uint64_t> requests;
};

Outcome runStrategy(const Options& options, const std::vector<Job>& jobs,
                    ReplayStrategy strategy) {
  ReplayOptions replay = options.replay;
  replay.strategy = strategy;

  std::vector<std::unique_ptr<Cache>> caches;
  caches.reserve(jobs.size());
  for (const Job& job : jobs) caches.push_back(buildCache(job, options.params));

  std::vector<ReplayRunner::Consumer> consumers;
  consumers.reserve(jobs.size());
  for (std::unique_ptr<Cache>& cache : caches) {
    Cache* raw = cache.get();
    consumers.emplace_back([raw](std::span<const Request> batch) {
      // The same batched path the single-threaded benchmark uses, so the
      // comparison is of delivery and nothing else.
      raw->accessBatch(batch.data(), batch.size());
    });
  }

  ReplayRunner runner(options.trace, replay);
  Outcome outcome;
  outcome.stats = runner.run(std::move(consumers));
  for (const std::unique_ptr<Cache>& cache : caches) {
    outcome.misses.push_back(cache->stats().nMiss());
    outcome.requests.push_back(cache->stats().n_req);
  }
  return outcome;
}

void report(const std::vector<Job>& jobs, ReplayStrategy strategy, const Outcome& outcome) {
  const ReplayStats& stats = outcome.stats;
  const std::uint64_t total_requests =
      std::accumulate(outcome.requests.begin(), outcome.requests.end(), std::uint64_t{0});
  std::printf("%-16s %8.2fs  %8.2f M req/s aggregate  %7.2f M req/s per config",
              std::string(replayStrategyName(stats.strategy_used)).c_str(), stats.wall_seconds,
              static_cast<double>(total_requests) / stats.wall_seconds / 1e6,
              static_cast<double>(outcome.requests.empty() ? 0 : outcome.requests[0]) /
                  stats.wall_seconds / 1e6);
  if (stats.reader_seconds > 0.0) {
    std::printf("  reader %5.2fs", stats.reader_seconds);
  }
  std::printf("  stalls r=%llu c=%llu\n", static_cast<unsigned long long>(stats.reader_stalls),
              static_cast<unsigned long long>(stats.consumer_stalls));
  (void)jobs;
  (void)strategy;
}

}  // namespace

int main(int argc, char** argv) {
  const Options options = parse(argc, argv);

  std::vector<Job> jobs;
  for (const std::string& policy : options.policies) {
    for (const std::uint64_t size : options.sizes) jobs.push_back(Job{policy, size});
  }

  std::printf("%zu configurations, ring %llu MiB, stride %llu, barrier batch %zu\n", jobs.size(),
              static_cast<unsigned long long>(options.replay.ring_bytes / (1024 * 1024)),
              static_cast<unsigned long long>(options.replay.publish_stride),
              options.replay.batch_requests);
  std::printf("%s\n", std::string(104, '-').c_str());

  try {
    std::vector<ReplayStrategy> strategies;
    if (options.all_strategies) {
      strategies = {ReplayStrategy::kMaterialized, ReplayStrategy::kSharedRing,
                    ReplayStrategy::kBatchBarrier, ReplayStrategy::kPrivateReaders};
    } else {
      strategies = {options.replay.strategy};
    }

    std::vector<std::uint64_t> reference_misses;
    std::string reference_name;
    for (const ReplayStrategy strategy : strategies) {
      Outcome outcome;
      for (int round = 0; round < options.repeat; ++round) {
        outcome = runStrategy(options, jobs, strategy);
      }
      report(jobs, strategy, outcome);

      // Every strategy delivers the same trace, so the miss counts must be
      // identical. A mismatch is a delivery bug.
      if (reference_misses.empty()) {
        reference_misses = outcome.misses;
        reference_name = replayStrategyName(outcome.stats.strategy_used);
      } else if (outcome.misses != reference_misses) {
        std::printf("  !! miss counts differ from %s:\n", reference_name.c_str());
        for (std::size_t i = 0; i < jobs.size(); ++i) {
          if (outcome.misses[i] != reference_misses[i]) {
            std::printf("     %s @ %llu: %llu vs %llu\n", jobs[i].policy.c_str(),
                        static_cast<unsigned long long>(jobs[i].size),
                        static_cast<unsigned long long>(outcome.misses[i]),
                        static_cast<unsigned long long>(reference_misses[i]));
          }
        }
        return 1;
      }
    }
    if (strategies.size() > 1) {
      std::printf("\nall %zu strategies produced identical miss counts across %zu "
                  "configurations\n",
                  strategies.size(), jobs.size());
    }
  } catch (const Error& error) {
    fail(error.what());
  }
  return 0;
}
