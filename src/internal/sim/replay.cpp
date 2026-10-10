#include "internal/sim/replay.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <new>
#include <thread>
#include <utility>

#include "internal/common/compiler.hpp"
#include "internal/common/error.hpp"
#include "internal/common/string_util.hpp"

namespace cachesim {
namespace {

double seconds(std::chrono::steady_clock::time_point start) {
  const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
  return elapsed.count();
}

std::size_t roundDownToPowerOfTwo(std::size_t value) {
  std::size_t power = 1;
  while (power <= value / 2) power <<= 1;
  return power;
}

// ---------------------------------------------------------------------------
// The shared ring
// ---------------------------------------------------------------------------

// One reader fills it; N consumers walk it behind their own cursors.
//
// Positions are *absolute* counts of requests since the start of the trace,
// never wrapped indices. Wrapping happens only when a position is turned into
// a slot (`& mask_`). That removes the usual ring-buffer ambiguity between
// "empty" and "full" entirely, and it makes every comparison below a plain
// subtraction on monotonically increasing numbers.
//
// The invariant the whole thing rests on:
//
//     write_frontier - min(consumer cursors) <= capacity
//
// i.e. the reader may only ever be a full ring ahead of the slowest consumer.
// Everything else is bookkeeping to maintain that while touching shared
// atomics as rarely as possible.
class SharedRing {
 public:
  SharedRing(std::size_t capacity, std::size_t consumer_count, std::uint64_t publish_stride)
      : ring_(capacity),
        mask_(capacity - 1),
        capacity_(capacity),
        publish_stride_(publish_stride),
        cursors_(consumer_count),
        reader_positions_(consumer_count, 0),
        reader_published_(consumer_count, 0) {}

  // ----------------------------------------------------------- reader side

  // Hands back a writable run of up to `max` slots at the write frontier,
  // blocking until that much space is free. An empty span means every
  // consumer has retired and there is nobody left to write for.
  std::span<Request> acquireWrite(std::size_t max) {
    std::uint64_t slowest = slowestCursor();
    while (write_frontier_ - slowest >= capacity_) {
      // Publish before waiting, or consumers may be idle waiting for data
      // this reader is holding back -- which would be a deadlock rather than
      // a slowdown.
      publishFrontier();
      ++stalls_;
      std::unique_lock<std::mutex> lock(mutex_);
      slowest = slowestCursor();
      if (slowest == kRetired) return {};
      if (write_frontier_ - slowest < capacity_) break;
      space_available_.wait(lock);
      slowest = slowestCursor();
      if (slowest == kRetired) return {};
    }
    if (slowest == kRetired) return {};

    const std::size_t free_slots =
        static_cast<std::size_t>(capacity_ - (write_frontier_ - slowest));
    const std::size_t slot = static_cast<std::size_t>(write_frontier_ & mask_);
    const std::size_t run = std::min({max, free_slots, capacity_ - slot});
    return {ring_.data() + slot, run};
  }

  void commitWrite(std::size_t count) {
    write_frontier_ += count;
    if (write_frontier_ - last_published_frontier_ >= publish_stride_) publishFrontier();
  }

  // No more requests will be written. Publishes everything and wakes every
  // consumer so each can see the final frontier and stop.
  void finish() {
    publishFrontier();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      eof_ = true;
    }
    data_available_.notify_all();
  }

  // ---------------------------------------------------------- consumer side

  // The next run of requests consumer `index` may read, blocking until there
  // is one. An empty span means the trace is finished.
  std::span<const Request> acquireRead(std::size_t index) {
    const std::uint64_t position = reader_positions_[index];
    std::uint64_t frontier = published_frontier_.load(std::memory_order_acquire);
    while (position == frontier) {
      // Publishing before blocking means the reader always has an up-to-date
      // view of this consumer when it matters -- which is exactly when this
      // consumer has caught up and is therefore not the one holding the
      // reader back. Cheap insurance on a path that is already slow.
      publishCursor(index);
      consumer_stalls_.fetch_add(1, std::memory_order_relaxed);
      std::unique_lock<std::mutex> lock(mutex_);
      frontier = published_frontier_.load(std::memory_order_acquire);
      if (position != frontier) break;
      if (eof_) return {};
      data_available_.wait(lock);
      frontier = published_frontier_.load(std::memory_order_acquire);
    }

    const std::size_t slot = static_cast<std::size_t>(position & mask_);
    const std::size_t run =
        static_cast<std::size_t>(std::min<std::uint64_t>(frontier - position, capacity_ - slot));
    return {ring_.data() + slot, run};
  }

  void commitRead(std::size_t index, std::size_t count) {
    reader_positions_[index] += count;
    if (reader_positions_[index] - reader_published_[index] >= publish_stride_) {
      publishCursor(index);
    }
  }

  // This consumer is done and should stop constraining the reader.
  void retire(std::size_t index) {
    cursors_[index].position.store(kRetired, std::memory_order_release);
    space_available_.notify_all();
  }

  [[nodiscard]] std::uint64_t readerStalls() const { return stalls_; }
  [[nodiscard]] std::uint64_t consumerStalls() const {
    return consumer_stalls_.load(std::memory_order_relaxed);
  }

 private:
  // A retired consumer's cursor, chosen so that it never constrains the
  // reader and is distinguishable from any real position.
  static constexpr std::uint64_t kRetired = std::numeric_limits<std::uint64_t>::max();

  // Each consumer's published cursor gets its own cache line. Without the
  // padding, N consumers' cursors share lines and every publication
  // invalidates the line in every other core that is reading it -- which is
  // precisely the contention the coarse publication exists to avoid, just
  // moved somewhere less obvious.
  struct alignas(kCacheLineSize) Cursor {
    std::atomic<std::uint64_t> position{0};
  };

  void publishFrontier() {
    published_frontier_.store(write_frontier_, std::memory_order_release);
    last_published_frontier_ = write_frontier_;
    data_available_.notify_all();
  }

  void publishCursor(std::size_t index) {
    cursors_[index].position.store(reader_positions_[index], std::memory_order_release);
    reader_published_[index] = reader_positions_[index];
    space_available_.notify_all();
  }

  // The slowest consumer's published position, or kRetired if they have all
  // retired. Scanned rather than maintained incrementally: it is read once
  // per publish_stride worth of requests, and N is the number of
  // configurations in a sweep -- tens, not millions.
  [[nodiscard]] std::uint64_t slowestCursor() const {
    std::uint64_t slowest = kRetired;
    for (const Cursor& cursor : cursors_) {
      const std::uint64_t position = cursor.position.load(std::memory_order_acquire);
      if (position < slowest) slowest = position;
    }
    return slowest;
  }

  std::vector<Request> ring_;
  std::uint64_t mask_;
  std::uint64_t capacity_;
  std::uint64_t publish_stride_;

  // Written by the reader, read by consumers.
  alignas(kCacheLineSize) std::atomic<std::uint64_t> published_frontier_{0};

  // Reader-private: the true frontier, which runs ahead of what is published.
  std::uint64_t write_frontier_ = 0;
  std::uint64_t last_published_frontier_ = 0;
  std::uint64_t stalls_ = 0;

  std::vector<Cursor> cursors_;
  // Consumer-private positions and last-published values. Indexed by
  // consumer, and only ever touched by that consumer.
  std::vector<std::uint64_t> reader_positions_;
  std::vector<std::uint64_t> reader_published_;

  alignas(kCacheLineSize) std::atomic<std::uint64_t> consumer_stalls_{0};

  // Only taken on the slow paths: a consumer with nothing to read, or a
  // reader with nowhere to write. At a publication stride of a million
  // requests these are rare enough that a condition variable costs nothing,
  // and it beats spinning when a sweep is already using every core.
  std::mutex mutex_;
  std::condition_variable space_available_;
  std::condition_variable data_available_;
  bool eof_ = false;
};

}  // namespace

std::string_view replayStrategyName(ReplayStrategy strategy) {
  switch (strategy) {
    case ReplayStrategy::kSharedRing: return "shared-ring";
    case ReplayStrategy::kBatchBarrier: return "batch-barrier";
    case ReplayStrategy::kPrivateReaders: return "private-readers";
    case ReplayStrategy::kMaterialized: return "materialized";
  }
  return "unknown";
}

ReplayStrategy parseReplayStrategy(std::string_view name) {
  const std::string key = normalizeKey(name);
  if (key == "sharedring" || key == "ring") return ReplayStrategy::kSharedRing;
  if (key == "batchbarrier" || key == "barrier" || key == "libcachesim") {
    return ReplayStrategy::kBatchBarrier;
  }
  if (key == "privatereaders" || key == "private") return ReplayStrategy::kPrivateReaders;
  if (key == "materialized" || key == "memory") return ReplayStrategy::kMaterialized;
  throw ConfigError("unknown replay strategy '" + std::string(name) +
                    "'; available: shared-ring, batch-barrier, private-readers, materialized");
}

ReplayRunner::ReplayRunner(TraceSpec spec, ReplayOptions options)
    : spec_(std::move(spec)), options_(options) {}

ReplayStats ReplayRunner::run(std::vector<Consumer> consumers) {
  if (consumers.empty()) throw ConfigError("replay needs at least one consumer");

  ReplayStrategy strategy = options_.strategy;
  if (strategy == ReplayStrategy::kSharedRing && options_.prefer_materialized_when_it_fits) {
    // If the ring is bigger than the trace it can never wrap, so every
    // consumer would be reading from a buffer that is simply the whole trace
    // -- with a reader thread and two atomics per stride for no reason.
    std::unique_ptr<ITraceReader> probe = openTrace(spec_);
    const std::uint64_t estimate = probe->estimatedRequests();
    if (estimate > 0 && estimate * sizeof(Request) <= options_.ring_bytes) {
      strategy = ReplayStrategy::kMaterialized;
    }
  }

  ReplayStats stats;
  switch (strategy) {
    case ReplayStrategy::kSharedRing: stats = runSharedRing(consumers); break;
    case ReplayStrategy::kBatchBarrier: stats = runBatchBarrier(consumers); break;
    case ReplayStrategy::kPrivateReaders: stats = runPrivateReaders(consumers); break;
    case ReplayStrategy::kMaterialized: stats = runMaterialized(consumers); break;
  }
  stats.strategy_used = strategy;
  return stats;
}

ReplayStats ReplayRunner::runSharedRing(std::vector<Consumer>& consumers) {
  const std::size_t requested = static_cast<std::size_t>(
      std::max<std::uint64_t>(options_.ring_bytes / sizeof(Request), 1024));
  std::size_t capacity = roundDownToPowerOfTwo(requested);

  // The ring has to be comfortably larger than the publication stride.
  //
  // Both sides run ahead of what they have published -- by up to one stride
  // each -- so a ring of only a stride or two could look full to the reader
  // while being empty to a consumer, and both would wait. Four strides is
  // enough that it cannot happen, and in any realistic configuration the
  // ring is thousands of strides anyway.
  std::uint64_t stride = options_.publish_stride;
  if (stride == 0) stride = 1;
  if (stride * 4 > capacity) stride = std::max<std::uint64_t>(capacity / 4, 1);

  SharedRing ring(capacity, consumers.size(), stride);
  std::vector<std::uint64_t> delivered(consumers.size(), 0);

  const auto wall_start = std::chrono::steady_clock::now();
  double reader_seconds = 0.0;

  std::thread reader([&] {
    const auto reader_start = std::chrono::steady_clock::now();
    try {
      std::unique_ptr<ITraceReader> trace = openTrace(spec_);
      for (;;) {
        const std::span<Request> slots = ring.acquireWrite(options_.read_batch);
        if (slots.empty()) break;  // every consumer retired
        const std::size_t produced = trace->nextBatch(slots.data(), slots.size());
        if (produced == 0) break;
        ring.commitWrite(produced);
      }
    } catch (...) {
      // Make sure consumers are released before the exception escapes, or
      // they wait forever on a reader that is no longer running.
      ring.finish();
      reader_seconds = seconds(reader_start);
      throw;
    }
    ring.finish();
    reader_seconds = seconds(reader_start);
  });

  std::vector<std::thread> workers;
  workers.reserve(consumers.size());
  for (std::size_t i = 0; i < consumers.size(); ++i) {
    workers.emplace_back([&, i] {
      for (;;) {
        const std::span<const Request> batch = ring.acquireRead(i);
        if (batch.empty()) break;
        consumers[i](batch);
        delivered[i] += batch.size();
        ring.commitRead(i, batch.size());
      }
      ring.retire(i);
    });
  }

  for (std::thread& worker : workers) worker.join();
  reader.join();

  ReplayStats stats;
  stats.wall_seconds = seconds(wall_start);
  stats.delivered = std::move(delivered);
  stats.reader_stalls = ring.readerStalls();
  stats.consumer_stalls = ring.consumerStalls();
  stats.reader_seconds = reader_seconds;
  return stats;
}

ReplayStats ReplayRunner::runMaterialized(std::vector<Consumer>& consumers) {
  const auto wall_start = std::chrono::steady_clock::now();
  const auto load_start = std::chrono::steady_clock::now();
  const std::vector<Request> trace = materializeTrace(spec_);
  const double load_seconds = seconds(load_start);

  std::vector<std::uint64_t> delivered(consumers.size(), 0);
  // A span size with no synchronization behind it; it only bounds how much a
  // consumer is handed at once.
  constexpr std::size_t kSpan = 8192;

  std::vector<std::thread> workers;
  workers.reserve(consumers.size());
  for (std::size_t i = 0; i < consumers.size(); ++i) {
    workers.emplace_back([&, i] {
      for (std::size_t offset = 0; offset < trace.size(); offset += kSpan) {
        const std::size_t run = std::min(kSpan, trace.size() - offset);
        consumers[i](std::span<const Request>(trace.data() + offset, run));
        delivered[i] += run;
      }
    });
  }
  for (std::thread& worker : workers) worker.join();

  ReplayStats stats;
  stats.wall_seconds = seconds(wall_start);
  stats.delivered = std::move(delivered);
  stats.reader_seconds = load_seconds;
  return stats;
}

// A faithful reproduction of libCacheSim's batch-and-barrier queue, so that
// what it costs can be measured rather than argued about.
//
// The reader fills one batch, waits for every consumer to report its previous
// batch finished, copies the batch into each consumer's private buffer, and
// releases them all. Two costs follow from the shape and neither can be tuned
// away:
//
//   * the barrier. No consumer gets batch k+1 until *every* consumer has
//     finished batch k, so each batch takes as long as the slowest
//     configuration in the sweep and the reader is idle for the difference.
//   * the copies. Every request is memcpy'd once per configuration, because
//     each consumer reads from its own buffer rather than from shared
//     memory. With 70 configurations that is 70 copies of the trace.
//
// Kept here, rather than only described, because "the barrier costs you X"
// is a claim that should be checkable.
ReplayStats ReplayRunner::runBatchBarrier(std::vector<Consumer>& consumers) {
  const std::size_t depth = std::max<std::size_t>(options_.batch_requests, 1);
  const std::size_t count = consumers.size();

  struct Slot {
    std::vector<Request> buffer;
    std::size_t size = 0;
    bool closed = false;
    // Binary handshake: `ready` is posted by the reader when a batch has been
    // copied in, `drained` by the consumer when it has finished with it.
    std::mutex mutex;
    std::condition_variable ready_cv;
    std::condition_variable drained_cv;
    bool ready = false;
    bool drained = true;
  };
  std::vector<std::unique_ptr<Slot>> slots;
  slots.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    slots.push_back(std::make_unique<Slot>());
    slots.back()->buffer.resize(depth);
  }

  std::vector<std::uint64_t> delivered(count, 0);
  std::atomic<std::uint64_t> barrier_waits{0};

  const auto wall_start = std::chrono::steady_clock::now();

  std::vector<std::thread> workers;
  workers.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    workers.emplace_back([&, i] {
      Slot& slot = *slots[i];
      for (;;) {
        std::size_t size = 0;
        {
          std::unique_lock<std::mutex> lock(slot.mutex);
          slot.ready_cv.wait(lock, [&] { return slot.ready || slot.closed; });
          if (!slot.ready && slot.closed) break;
          size = slot.size;
          slot.ready = false;
        }
        if (size > 0) {
          consumers[i](std::span<const Request>(slot.buffer.data(), size));
          delivered[i] += size;
        }
        {
          std::lock_guard<std::mutex> lock(slot.mutex);
          slot.drained = true;
        }
        slot.drained_cv.notify_one();
        if (size < depth) break;  // a short batch is end of trace
      }
    });
  }

  double reader_seconds = 0.0;
  {
    const auto reader_start = std::chrono::steady_clock::now();
    std::unique_ptr<ITraceReader> trace = openTrace(spec_);
    std::vector<Request> batch(depth);
    for (;;) {
      const std::size_t produced = trace->nextBatch(batch.data(), depth);
      if (produced == 0) break;

      // The barrier: nobody advances until everybody has finished.
      for (std::size_t i = 0; i < count; ++i) {
        Slot& slot = *slots[i];
        std::unique_lock<std::mutex> lock(slot.mutex);
        if (!slot.drained) barrier_waits.fetch_add(1, std::memory_order_relaxed);
        slot.drained_cv.wait(lock, [&] { return slot.drained; });
      }

      // The copies: one per configuration.
      for (std::size_t i = 0; i < count; ++i) {
        Slot& slot = *slots[i];
        {
          std::lock_guard<std::mutex> lock(slot.mutex);
          std::copy_n(batch.data(), produced, slot.buffer.data());
          slot.size = produced;
          slot.ready = true;
          slot.drained = false;
        }
        slot.ready_cv.notify_one();
      }
      if (produced < depth) break;
    }

    for (std::size_t i = 0; i < count; ++i) {
      Slot& slot = *slots[i];
      {
        std::unique_lock<std::mutex> lock(slot.mutex);
        slot.drained_cv.wait(lock, [&] { return slot.drained; });
        slot.closed = true;
      }
      slot.ready_cv.notify_one();
    }
    reader_seconds = seconds(reader_start);
  }

  for (std::thread& worker : workers) worker.join();

  ReplayStats stats;
  stats.wall_seconds = seconds(wall_start);
  stats.delivered = std::move(delivered);
  stats.reader_stalls = barrier_waits.load(std::memory_order_relaxed);
  stats.reader_seconds = reader_seconds;
  return stats;
}

ReplayStats ReplayRunner::runPrivateReaders(std::vector<Consumer>& consumers) {
  const auto wall_start = std::chrono::steady_clock::now();
  std::vector<std::uint64_t> delivered(consumers.size(), 0);

  std::vector<std::thread> workers;
  workers.reserve(consumers.size());
  for (std::size_t i = 0; i < consumers.size(); ++i) {
    workers.emplace_back([&, i] {
      std::unique_ptr<ITraceReader> trace = openTrace(spec_);
      std::vector<Request> batch(options_.read_batch);
      for (;;) {
        const std::size_t produced = trace->nextBatch(batch.data(), batch.size());
        if (produced == 0) break;
        consumers[i](std::span<const Request>(batch.data(), produced));
        delivered[i] += produced;
      }
    });
  }
  for (std::thread& worker : workers) worker.join();

  ReplayStats stats;
  stats.wall_seconds = seconds(wall_start);
  stats.delivered = std::move(delivered);
  return stats;
}

}  // namespace cachesim
