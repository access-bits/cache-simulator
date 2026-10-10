#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "internal/request/request.hpp"
#include "internal/trace/trace_reader.hpp"

namespace cachesim {

// How one trace gets delivered to several simulations running concurrently.
//
// This is the part of a parameter sweep that decides whether it scales. Each
// configuration is a self-contained cache that shares nothing with the
// others, so the only coupling left is the trace they all have to read -- and
// how that coupling is arranged is worth more than anything inside the
// engine once there are more than a handful of configurations.
enum class ReplayStrategy {
  // One reader thread fills a large shared ring; each consumer walks it at
  // its own pace behind its own cursor. The reader may only overwrite slots
  // the slowest consumer has passed, so the ring's capacity *is* the slack
  // allowed between fastest and slowest. Cursors are published coarsely, so
  // the shared atomics are touched once per million-odd requests rather than
  // per request.
  //
  // The default, and the only one of these that both decodes the trace once
  // and lets configurations run at different speeds.
  kSharedRing,

  // What libCacheSim does today, reproduced so the comparison is honest: the
  // reader fills a batch, waits for *every* consumer to finish the previous
  // one, copies the batch into each consumer's private buffer, and releases
  // them. Two costs, both structural -- the slowest consumer gates all of
  // them at every batch boundary, and every request is copied once per
  // configuration.
  kBatchBarrier,

  // Each consumer opens its own reader and streams the file independently.
  // No synchronization at all, but the trace is decoded N times, and for a
  // compressed trace that is N decompressions.
  kPrivateReaders,

  // Decode the whole trace once into a shared read-only array, then let every
  // consumer walk it with no synchronization whatsoever.
  //
  // Strictly the fastest when the trace fits in memory, and the thing the
  // ring degenerates to when its capacity exceeds the trace length -- so it
  // is the right answer rather than a baseline, whenever it is affordable.
  // At 32 bytes a request, a billion requests is 32 GB.
  kMaterialized,
};

[[nodiscard]] std::string_view replayStrategyName(ReplayStrategy strategy);
[[nodiscard]] ReplayStrategy parseReplayStrategy(std::string_view name);

struct ReplayOptions {
  ReplayStrategy strategy = ReplayStrategy::kSharedRing;

  // Shared ring capacity, in bytes. Rounded down to a power-of-two number of
  // requests. This is the memory budget the whole sweep gets for buffering,
  // and it buys slack between the fastest and slowest configuration: at 32
  // bytes a request, 128 GiB is about 4.3 billion requests of slack.
  std::uint64_t ring_bytes = 1ULL << 30;

  // How many requests a consumer gets through before publishing its cursor,
  // and the reader before publishing its write frontier.
  //
  // The whole point of the coarse publication: a cursor written per request
  // is a contended cache line bouncing between every core, and a wakeup per
  // request is worse. Larger is cheaper but coarser -- a consumer cannot see
  // data the reader has not published, so a very large stride leaves
  // consumers idle in bursts while the reader holds finished work back.
  std::uint64_t publish_stride = 1ULL << 20;

  // Requests the reader decodes per call into the ring.
  std::size_t read_batch = 8192;

  // Batch depth for kBatchBarrier, matching libCacheSim's queue_depth.
  std::size_t batch_requests = 32768;

  // Let kMaterialized be chosen automatically when the trace is known to fit
  // in `ring_bytes`, since the ring would never wrap and its machinery would
  // be pure overhead.
  bool prefer_materialized_when_it_fits = true;
};

struct ReplayStats {
  double wall_seconds = 0.0;

  // Requests delivered to each consumer. Every consumer sees the same trace,
  // so these should all be equal; they are reported per consumer because a
  // mismatch is the signature of a delivery bug.
  std::vector<std::uint64_t> delivered;

  // Times the reader had to wait for the slowest consumer to free ring
  // space. Nonzero means the ring is too small for the spread between
  // configurations -- which is the one number that says whether the budget
  // needs raising.
  std::uint64_t reader_stalls = 0;

  // Times a consumer had to wait for the reader to publish more data.
  // Dominant when the reader is the bottleneck, which it becomes as soon as
  // there are more consumers than it can feed.
  std::uint64_t consumer_stalls = 0;

  // Seconds the reader thread spent decoding, where there is a reader
  // thread. Compare against wall_seconds to see whether the reader or the
  // consumers are the limit.
  double reader_seconds = 0.0;

  ReplayStrategy strategy_used = ReplayStrategy::kSharedRing;
};

// Replays one trace against several consumers concurrently.
//
// Each consumer is called with spans of requests, in trace order, exactly
// once each, on its own thread. A consumer must not retain the span past the
// call: with the shared ring, those slots are reused once its cursor moves.
//
// Consumers are independent -- nothing here synchronizes them with each
// other, which is the point.
class ReplayRunner {
 public:
  using Consumer = std::function<void(std::span<const Request>)>;

  ReplayRunner(TraceSpec spec, ReplayOptions options);

  [[nodiscard]] ReplayStats run(std::vector<Consumer> consumers);

 private:
  [[nodiscard]] ReplayStats runSharedRing(std::vector<Consumer>& consumers);
  [[nodiscard]] ReplayStats runBatchBarrier(std::vector<Consumer>& consumers);
  [[nodiscard]] ReplayStats runPrivateReaders(std::vector<Consumer>& consumers);
  [[nodiscard]] ReplayStats runMaterialized(std::vector<Consumer>& consumers);

  TraceSpec spec_;
  ReplayOptions options_;
};

}  // namespace cachesim
