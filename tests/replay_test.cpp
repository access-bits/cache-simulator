// Tests for the trace delivery strategies.
//
// The property that matters is the same for all four: every consumer must see
// exactly the trace, in order, once. A delivery bug does not crash -- it
// silently hands one configuration a slightly different trace from another,
// and the sweep's rows stop being comparable with nothing to indicate it.
//
// The shared ring is the one with real concurrency in it, so most of this
// file is about forcing it into the states that are easy to get wrong: a ring
// far smaller than the trace so it wraps hundreds of times, consumers running
// at deliberately different speeds so the reader has to wait on the slowest,
// and a consumer that retires early so it stops constraining the others.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#include "internal/common/error.hpp"
#include "internal/sim/replay.hpp"
#include "internal/trace/trace_reader.hpp"
#include "tests/test_support.hpp"

using namespace cachesim;

namespace {

// Writes a trace in oracleGeneral format whose object ids are 0, 1, 2, ...
// so that a consumer can verify both the contents and the order by checking
// each id against a counter -- a reordering, a duplicate or a gap all show up
// immediately, which a Zipf trace would hide.
std::string writeCountingTrace(const std::string& path, std::uint64_t count) {
#pragma pack(push, 1)
  struct Record {
    std::uint32_t clock_time;
    std::uint64_t obj_id;
    std::uint32_t obj_size;
    std::int64_t next_access_vtime;
  };
#pragma pack(pop)
  static_assert(sizeof(Record) == 24);

  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) throw TraceError("cannot write test trace " + path);
  std::vector<Record> batch;
  batch.reserve(4096);
  for (std::uint64_t i = 0; i < count; ++i) {
    batch.push_back(Record{static_cast<std::uint32_t>(i / 1000), i, 1, -1});
    if (batch.size() == 4096 || i + 1 == count) {
      std::fwrite(batch.data(), sizeof(Record), batch.size(), file);
      batch.clear();
    }
  }
  std::fclose(file);
  return path;
}

TraceSpec specFor(const std::string& path) {
  TraceSpec spec;
  spec.path = path;
  spec.format = TraceFormat::kOracleGeneral;
  return spec;
}

// Checks that it is handed 0, 1, 2, ... exactly once each, and counts how
// many it saw.
struct CountingConsumer {
  std::uint64_t expected_next = 0;
  std::uint64_t seen = 0;
  std::uint64_t order_violations = 0;
  // Microseconds to sleep per span, to make one consumer slower than another.
  int slow_micros = 0;

  void operator()(std::span<const Request> batch) {
    for (const Request& req : batch) {
      if (req.obj_id != expected_next) ++order_violations;
      ++expected_next;
      ++seen;
    }
    if (slow_micros > 0) std::this_thread::sleep_for(std::chrono::microseconds(slow_micros));
  }
};

void testEveryStrategyDeliversTheWholeTrace(const std::string& path, std::uint64_t count) {
  for (const ReplayStrategy strategy :
       {ReplayStrategy::kSharedRing, ReplayStrategy::kBatchBarrier,
        ReplayStrategy::kPrivateReaders, ReplayStrategy::kMaterialized}) {
    const std::string name(replayStrategyName(strategy));
    TEST_CASE(name + ": four consumers each see the whole trace in order") {
      ReplayOptions options;
      options.strategy = strategy;
      // Deliberately tiny, so the ring wraps many times over rather than
      // holding the trace and never exercising the wrap at all.
      options.ring_bytes = 64 * 1024;
      options.publish_stride = 128;
      options.read_batch = 300;
      options.batch_requests = 512;
      options.prefer_materialized_when_it_fits = false;

      std::vector<CountingConsumer> state(4);
      std::vector<ReplayRunner::Consumer> consumers;
      for (CountingConsumer& consumer : state) {
        consumers.emplace_back([&consumer](std::span<const Request> batch) { consumer(batch); });
      }

      ReplayRunner runner(specFor(path), options);
      const ReplayStats stats = runner.run(std::move(consumers));

      CHECK_EQ(stats.delivered.size(), std::size_t{4});
      for (std::size_t i = 0; i < state.size(); ++i) {
        CHECK_EQ(state[i].seen, count);
        CHECK_EQ(state[i].order_violations, std::uint64_t{0});
        CHECK_EQ(stats.delivered[i], count);
      }
    }
  }
}

void testRingUnderUnevenConsumers(const std::string& path, std::uint64_t count) {
  TEST_CASE("shared ring: a slow consumer does not corrupt a fast one") {
    // This is the case the design exists for, and the one most likely to go
    // wrong: the ring must wrap repeatedly while one consumer is far behind,
    // and the reader must never overwrite a slot that consumer has not read.
    ReplayOptions options;
    options.strategy = ReplayStrategy::kSharedRing;
    options.ring_bytes = 32 * 1024;  // 1024 requests
    options.publish_stride = 64;
    options.read_batch = 100;
    options.prefer_materialized_when_it_fits = false;

    std::vector<CountingConsumer> state(3);
    state[0].slow_micros = 0;    // as fast as it can
    state[1].slow_micros = 50;   // noticeably behind
    state[2].slow_micros = 200;  // the one the reader keeps waiting for

    std::vector<ReplayRunner::Consumer> consumers;
    for (CountingConsumer& consumer : state) {
      consumers.emplace_back([&consumer](std::span<const Request> batch) { consumer(batch); });
    }

    ReplayRunner runner(specFor(path), options);
    const ReplayStats stats = runner.run(std::move(consumers));

    for (const CountingConsumer& consumer : state) {
      CHECK_EQ(consumer.seen, count);
      CHECK_EQ(consumer.order_violations, std::uint64_t{0});
    }
    // The ring holds 1024 requests and the slowest consumer is far slower
    // than the reader, so the reader must have had to wait for space. If it
    // never did, the test is not exercising what it claims to.
    CHECK(stats.reader_stalls > 0);
  }

  TEST_CASE("shared ring: survives a ring smaller than one read batch") {
    ReplayOptions options;
    options.strategy = ReplayStrategy::kSharedRing;
    options.ring_bytes = 4096;  // 128 requests
    options.publish_stride = 1024;  // larger than the ring; must be clamped
    options.read_batch = 4096;      // larger than the ring
    options.prefer_materialized_when_it_fits = false;

    std::vector<CountingConsumer> state(2);
    std::vector<ReplayRunner::Consumer> consumers;
    for (CountingConsumer& consumer : state) {
      consumers.emplace_back([&consumer](std::span<const Request> batch) { consumer(batch); });
    }
    ReplayRunner runner(specFor(path), options);
    const ReplayStats stats = runner.run(std::move(consumers));
    (void)stats;
    for (const CountingConsumer& consumer : state) {
      CHECK_EQ(consumer.seen, count);
      CHECK_EQ(consumer.order_violations, std::uint64_t{0});
    }
  }

  TEST_CASE("shared ring: one consumer") {
    ReplayOptions options;
    options.strategy = ReplayStrategy::kSharedRing;
    options.ring_bytes = 32 * 1024;
    options.publish_stride = 64;
    options.prefer_materialized_when_it_fits = false;
    CountingConsumer consumer;
    std::vector<ReplayRunner::Consumer> consumers;
    consumers.emplace_back([&consumer](std::span<const Request> batch) { consumer(batch); });
    ReplayRunner runner(specFor(path), options);
    const ReplayStats stats = runner.run(std::move(consumers));
    CHECK_EQ(consumer.seen, count);
    CHECK_EQ(consumer.order_violations, std::uint64_t{0});
    CHECK_EQ(stats.delivered[0], count);
  }

  TEST_CASE("shared ring: many consumers, more than cores") {
    ReplayOptions options;
    options.strategy = ReplayStrategy::kSharedRing;
    options.ring_bytes = 128 * 1024;
    options.publish_stride = 256;
    options.prefer_materialized_when_it_fits = false;
    std::vector<CountingConsumer> state(17);
    std::vector<ReplayRunner::Consumer> consumers;
    for (CountingConsumer& consumer : state) {
      consumers.emplace_back([&consumer](std::span<const Request> batch) { consumer(batch); });
    }
    ReplayRunner runner(specFor(path), options);
    const ReplayStats stats = runner.run(std::move(consumers));
    for (std::size_t i = 0; i < state.size(); ++i) {
      CHECK_EQ(state[i].seen, count);
      CHECK_EQ(state[i].order_violations, std::uint64_t{0});
      CHECK_EQ(stats.delivered[i], count);
    }
  }
}

void testAutoMaterialize(const std::string& path, std::uint64_t count) {
  TEST_CASE("shared ring degenerates to materialized when the trace fits") {
    // A ring larger than the trace can never wrap, so the reader thread and
    // the cursor traffic would be pure overhead. The runner should notice.
    ReplayOptions options;
    options.strategy = ReplayStrategy::kSharedRing;
    options.ring_bytes = 1ULL << 30;  // far larger than the test trace
    options.prefer_materialized_when_it_fits = true;

    CountingConsumer consumer;
    std::vector<ReplayRunner::Consumer> consumers;
    consumers.emplace_back([&consumer](std::span<const Request> batch) { consumer(batch); });
    ReplayRunner runner(specFor(path), options);
    const ReplayStats stats = runner.run(std::move(consumers));
    CHECK(stats.strategy_used == ReplayStrategy::kMaterialized);
    CHECK_EQ(consumer.seen, count);
  }

  TEST_CASE("the choice can be forced off") {
    ReplayOptions options;
    options.strategy = ReplayStrategy::kSharedRing;
    options.ring_bytes = 1ULL << 30;
    options.prefer_materialized_when_it_fits = false;
    CountingConsumer consumer;
    std::vector<ReplayRunner::Consumer> consumers;
    consumers.emplace_back([&consumer](std::span<const Request> batch) { consumer(batch); });
    ReplayRunner runner(specFor(path), options);
    const ReplayStats stats = runner.run(std::move(consumers));
    CHECK(stats.strategy_used == ReplayStrategy::kSharedRing);
    CHECK_EQ(consumer.seen, count);
  }
}

void testNames() {
  TEST_CASE("strategy names round-trip") {
    for (const ReplayStrategy strategy :
         {ReplayStrategy::kSharedRing, ReplayStrategy::kBatchBarrier,
          ReplayStrategy::kPrivateReaders, ReplayStrategy::kMaterialized}) {
      CHECK(parseReplayStrategy(replayStrategyName(strategy)) == strategy);
    }
    CHECK(parseReplayStrategy("libcachesim") == ReplayStrategy::kBatchBarrier);
    CHECK(parseReplayStrategy("ring") == ReplayStrategy::kSharedRing);
    bool threw = false;
    try {
      (void)parseReplayStrategy("nonsense");
    } catch (const ConfigError&) {
      threw = true;
    }
    CHECK(threw);
  }

  TEST_CASE("no consumers is an error, not a hang") {
    ReplayOptions options;
    ReplayRunner runner(specFor("/nonexistent"), options);
    bool threw = false;
    try {
      (void)runner.run({});
    } catch (const ConfigError&) {
      threw = true;
    }
    CHECK(threw);
  }
}

}  // namespace

int main() {
  const std::string path = "/tmp/cachesim_replay_test.oracleGeneral";
  // Large enough that a 1024-request ring wraps about 100 times, small enough
  // that the suite still runs in a second.
  constexpr std::uint64_t kRequests = 120000;
  writeCountingTrace(path, kRequests);

  std::printf("Delivery completeness\n");
  testEveryStrategyDeliversTheWholeTrace(path, kRequests);
  std::printf("Shared ring under pressure\n");
  testRingUnderUnevenConsumers(path, kRequests);
  std::printf("Strategy selection\n");
  testAutoMaterialize(path, kRequests);
  std::printf("Plumbing\n");
  testNames();

  std::remove(path.c_str());
  return cachesim::test::summarize("replay_test");
}
