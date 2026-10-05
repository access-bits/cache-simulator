// Policy tests, in three layers.
//
// 1. Reference cross-validation. For every policy whose definition is short
//    enough to re-implement obviously-correctly, a deliberately naive model
//    is written here and the engine's hit count must match it request for
//    request. "Obviously correct" means O(n) scans and std::list — the
//    reference is allowed to be absurdly slow, that is the point.
// 2. Invariants that must hold for every policy, including the ones with no
//    tractable reference (S3FIFO, W-TinyLFU, Sieve): capacity is never
//    exceeded, every admitted object is tracked, no object is lost or
//    duplicated, and the cache survives clear/refill.
// 3. Known-good relationships: Belady is an upper bound on every online
//    policy, LRU-1 is LRU, and so on.

#include <algorithm>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "internal/cache/cache.hpp"
#include "internal/common/error.hpp"
#include "internal/common/hash.hpp"
#include "internal/eviction/policy_registry.hpp"
#include "policies/arc.hpp"
#include "tests/test_support.hpp"

using namespace cachesim;

namespace {

// ------------------------------------------------------------ trace building

// A Zipf-ish trace: squaring a uniform draw biases towards low ids, which
// gives the reuse structure a real cache sees. Small universes relative to
// the capacity are deliberate — they exercise the eviction paths hard.
std::vector<Request> makeTrace(std::size_t count, std::uint64_t universe, std::uint64_t seed,
                               bool variable_sizes = false) {
  Rng rng(seed);
  std::vector<Request> trace;
  trace.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    Request r;
    const double u = rng.nextDouble();
    r.obj_id = static_cast<std::uint64_t>(u * u * static_cast<double>(universe));
    r.size = variable_sizes ? static_cast<std::uint32_t>(1 + rng.below(64)) : 1;
    r.clock_time = static_cast<std::int64_t>(i);
    trace.push_back(r);
  }
  // Fill in the oracle column: the index of each object's next request.
  std::unordered_map<std::uint64_t, std::size_t> next_seen;
  for (std::size_t i = trace.size(); i-- > 0;) {
    const auto it = next_seen.find(trace[i].obj_id);
    trace[i].next_access_vtime =
        it == next_seen.end() ? kNeverAgain : static_cast<std::int64_t>(it->second);
    next_seen[trace[i].obj_id] = i;
  }
  return trace;
}

// A hot set, looped over `repeats` times, then a scan through objects that
// are never seen again — over and over.
//
// This is the workload that separates scan-resistant policies from LRU, and
// the reason it does is worth stating: LRU's recency ordering treats each
// scan object, seen once, as more valuable than a hot object seen a hundred
// times but slightly longer ago, so every scan flushes the entire working
// set. A policy that distinguishes "seen once" from "seen repeatedly" keeps
// the hot set and spends only part of the cache on the scan.
std::vector<Request> makeScanTrace(std::size_t loops, std::uint64_t hot_set,
                                   std::size_t repeats, std::uint64_t scan_length) {
  std::vector<Request> trace;
  std::uint64_t scan_id = 1'000'000;
  for (std::size_t loop = 0; loop < loops; ++loop) {
    for (std::size_t pass = 0; pass < repeats; ++pass) {
      for (std::uint64_t i = 0; i < hot_set; ++i) {
        Request r;
        r.obj_id = i;
        r.size = 1;
        r.clock_time = static_cast<std::int64_t>(trace.size());
        trace.push_back(r);
      }
    }
    for (std::uint64_t i = 0; i < scan_length; ++i) {
      Request r;
      r.obj_id = scan_id++;
      r.size = 1;
      r.clock_time = static_cast<std::int64_t>(trace.size());
      trace.push_back(r);
    }
  }
  return trace;
}

struct RunResult {
  std::uint64_t hits = 0;
  std::uint64_t evictions = 0;
  std::size_t resident = 0;
  std::uint64_t occupied = 0;
};

RunResult run(const std::string& policy, const std::vector<Request>& trace,
              std::uint64_t capacity, const std::string& params = {}) {
  PolicyConfig pc;
  pc.capacity_bytes = capacity;
  pc.entry_hint = static_cast<std::size_t>(capacity);
  pc.params = ParamMap(params);
  CacheOptions options;
  options.capacity_bytes = capacity;
  options.entry_hint = static_cast<std::size_t>(capacity);
  Cache cache(options, PolicyRegistry::instance().create(policy, pc));
  for (const Request& r : trace) {
    cache.access(r);
    // Checked on every request rather than at the end: an intermediate
    // overflow that later resolves is still a bug, and only a per-request
    // check catches it.
    if (cache.occupiedBytes() > capacity) {
      CHECK(cache.occupiedBytes() <= capacity);
      break;
    }
  }
  return RunResult{cache.stats().n_hit, cache.stats().n_evict, cache.objectCount(),
                   cache.occupiedBytes()};
}

// ------------------------------------------------------------- reference models

std::uint64_t refLru(const std::vector<Request>& trace, std::size_t capacity) {
  std::list<std::uint64_t> order;  // front = most recently used
  std::uint64_t hits = 0;
  for (const Request& r : trace) {
    const auto it = std::find(order.begin(), order.end(), r.obj_id);
    if (it != order.end()) {
      ++hits;
      order.erase(it);
      order.push_front(r.obj_id);
      continue;
    }
    if (order.size() == capacity) order.pop_back();
    order.push_front(r.obj_id);
  }
  return hits;
}

std::uint64_t refFifo(const std::vector<Request>& trace, std::size_t capacity) {
  std::deque<std::uint64_t> order;
  std::uint64_t hits = 0;
  for (const Request& r : trace) {
    if (std::find(order.begin(), order.end(), r.obj_id) != order.end()) {
      ++hits;
      continue;
    }
    if (order.size() == capacity) order.pop_front();
    order.push_back(r.obj_id);
  }
  return hits;
}

std::uint64_t refMru(const std::vector<Request>& trace, std::size_t capacity) {
  std::list<std::uint64_t> order;  // front = most recently used
  std::uint64_t hits = 0;
  for (const Request& r : trace) {
    const auto it = std::find(order.begin(), order.end(), r.obj_id);
    if (it != order.end()) {
      ++hits;
      order.erase(it);
      order.push_front(r.obj_id);
      continue;
    }
    if (order.size() == capacity) order.pop_front();
    order.push_front(r.obj_id);
  }
  return hits;
}

// Exact LFU, FIFO tie-break within a frequency.
std::uint64_t refLfu(const std::vector<Request>& trace, std::size_t capacity) {
  struct Item {
    std::int64_t freq;
    std::int64_t reached_freq_at;
  };
  std::unordered_map<std::uint64_t, Item> cached;
  std::int64_t tick = 0;
  std::uint64_t hits = 0;
  for (const Request& r : trace) {
    ++tick;
    const auto it = cached.find(r.obj_id);
    if (it != cached.end()) {
      ++hits;
      ++it->second.freq;
      it->second.reached_freq_at = tick;
      continue;
    }
    if (cached.size() == capacity) {
      auto victim = cached.begin();
      for (auto c = cached.begin(); c != cached.end(); ++c) {
        if (c->second.freq < victim->second.freq ||
            (c->second.freq == victim->second.freq &&
             c->second.reached_freq_at < victim->second.reached_freq_at)) {
          victim = c;
        }
      }
      cached.erase(victim);
    }
    cached[r.obj_id] = Item{1, tick};
  }
  return hits;
}

// LFU-DA: key = reference count + age, age rises to each victim's key, ties
// broken in favour of whichever object reached its current key first.
std::uint64_t refLfuDa(const std::vector<Request>& trace, std::size_t capacity) {
  struct Item {
    std::int64_t count;
    std::int64_t key;
    std::int64_t reached_key_at;
  };
  std::unordered_map<std::uint64_t, Item> cached;
  std::int64_t age = 0;
  std::int64_t tick = 0;
  std::uint64_t hits = 0;
  for (const Request& r : trace) {
    const auto it = cached.find(r.obj_id);
    if (it != cached.end()) {
      ++hits;
      ++it->second.count;
      it->second.key = age + it->second.count;
      it->second.reached_key_at = ++tick;
      continue;
    }
    if (cached.size() == capacity) {
      auto victim = cached.begin();
      for (auto c = cached.begin(); c != cached.end(); ++c) {
        if (std::pair{c->second.key, c->second.reached_key_at} <
            std::pair{victim->second.key, victim->second.reached_key_at}) {
          victim = c;
        }
      }
      age = victim->second.key;
      cached.erase(victim);
    }
    cached[r.obj_id] = Item{1, age + 1, ++tick};
  }
  return hits;
}

// Belady by its definition: for each eviction, scan the remaining trace.
std::uint64_t refBelady(const std::vector<Request>& trace, std::size_t capacity) {
  std::set<std::uint64_t> cached;
  std::uint64_t hits = 0;
  for (std::size_t i = 0; i < trace.size(); ++i) {
    const std::uint64_t id = trace[i].obj_id;
    if (cached.contains(id)) {
      ++hits;
      continue;
    }
    if (cached.size() == capacity) {
      std::uint64_t worst = *cached.begin();
      std::size_t worst_next = 0;
      for (const std::uint64_t candidate : cached) {
        std::size_t next = trace.size() + 1;
        for (std::size_t j = i + 1; j < trace.size(); ++j) {
          if (trace[j].obj_id == candidate) {
            next = j;
            break;
          }
        }
        if (next > worst_next) {
          worst_next = next;
          worst = candidate;
        }
      }
      cached.erase(worst);
    }
    cached.insert(id);
  }
  return hits;
}

// CLOCK with a one-bit reference counter, as a plain circular list.
std::uint64_t refClock(const std::vector<Request>& trace, std::size_t capacity) {
  struct Item {
    std::uint64_t id;
    bool referenced;
  };
  std::list<Item> ring;  // front = most recently inserted or given a chance
  std::uint64_t hits = 0;
  const auto find = [&](std::uint64_t id) {
    return std::find_if(ring.begin(), ring.end(), [&](const Item& i) { return i.id == id; });
  };
  for (const Request& r : trace) {
    const auto it = find(r.obj_id);
    if (it != ring.end()) {
      ++hits;
      it->referenced = true;
      continue;
    }
    if (ring.size() == capacity) {
      for (;;) {
        Item back = ring.back();
        ring.pop_back();
        if (!back.referenced) break;
        back.referenced = false;
        ring.push_front(back);
      }
    }
    ring.push_front(Item{r.obj_id, false});
  }
  return hits;
}

// SIEVE: the hand walks from the oldest towards the newest and never moves an
// object.
std::uint64_t refSieve(const std::vector<Request>& trace, std::size_t capacity) {
  struct Item {
    std::uint64_t id;
    bool visited;
  };
  std::list<Item> queue;  // front = newest
  auto hand = queue.end();
  std::uint64_t hits = 0;
  for (const Request& r : trace) {
    const auto found =
        std::find_if(queue.begin(), queue.end(), [&](const Item& i) { return i.id == r.obj_id; });
    if (found != queue.end()) {
      ++hits;
      found->visited = true;
      continue;
    }
    if (queue.size() == capacity) {
      auto candidate = hand == queue.end() ? std::prev(queue.end()) : hand;
      for (std::size_t steps = 0; steps < queue.size(); ++steps) {
        if (!candidate->visited) break;
        candidate->visited = false;
        candidate = candidate == queue.begin() ? std::prev(queue.end()) : std::prev(candidate);
      }
      hand = candidate == queue.begin() ? queue.end() : std::prev(candidate);
      queue.erase(candidate);
    }
    queue.push_front(Item{r.obj_id, false});
  }
  return hits;
}

// 2Q, with A1in as a FIFO, A1out as a ghost FIFO and Am as an LRU.
std::uint64_t refTwoQ(const std::vector<Request>& trace, std::size_t capacity, double kin,
                      double kout) {
  const std::size_t kin_size = static_cast<std::size_t>(static_cast<double>(capacity) * kin);
  const std::size_t kout_size =
      std::max<std::size_t>(16, static_cast<std::size_t>(static_cast<double>(capacity) * kout));
  std::deque<std::uint64_t> a1in;
  std::deque<std::uint64_t> a1out;
  std::list<std::uint64_t> am;  // front = MRU
  std::uint64_t hits = 0;
  const auto contains = [](const auto& container, std::uint64_t id) {
    return std::find(container.begin(), container.end(), id) != container.end();
  };
  for (const Request& r : trace) {
    const std::uint64_t id = r.obj_id;
    const auto in_am = std::find(am.begin(), am.end(), id);
    if (in_am != am.end()) {
      ++hits;
      am.erase(in_am);
      am.push_front(id);
      continue;
    }
    if (contains(a1in, id)) {
      ++hits;  // a hit in A1in earns nothing
      continue;
    }
    // Miss. The paper's order matters here: A1out membership is checked
    // *before* reclaiming, so the reclaim that follows cannot age this very
    // id out of A1out and turn a promotion into an ordinary admission.
    const auto in_a1out = std::find(a1out.begin(), a1out.end(), id);
    const bool promote = in_a1out != a1out.end();
    if (promote) a1out.erase(in_a1out);
    while (a1in.size() + am.size() >= capacity) {
      if (a1in.size() > kin_size || am.empty()) {
        const std::uint64_t victim = a1in.front();
        a1in.pop_front();
        a1out.push_back(victim);
        if (a1out.size() > kout_size) a1out.pop_front();
      } else {
        am.pop_back();
      }
    }
    if (promote) {
      am.push_front(id);
    } else {
      a1in.push_back(id);
    }
  }
  return hits;
}

// LRU-K: victim is the object whose K-th most recent reference is oldest;
// fewer than K references means "infinitely old", ordered among themselves by
// most recent reference.
std::uint64_t refLruK(const std::vector<Request>& trace, std::size_t capacity, std::size_t k) {
  struct Item {
    std::deque<std::int64_t> times;  // newest first
  };
  std::unordered_map<std::uint64_t, Item> cached;
  std::int64_t clock = 0;
  std::uint64_t hits = 0;
  const auto record = [&](Item& item, std::int64_t now) {
    item.times.push_front(now);
    while (item.times.size() > k) item.times.pop_back();
  };
  for (const Request& r : trace) {
    ++clock;
    const auto it = cached.find(r.obj_id);
    if (it != cached.end()) {
      ++hits;
      record(it->second, clock);
      continue;
    }
    if (cached.size() == capacity) {
      auto victim = cached.end();
      std::pair<std::int64_t, std::int64_t> worst{0, 0};
      for (auto c = cached.begin(); c != cached.end(); ++c) {
        const std::int64_t kth = c->second.times.size() >= k ? c->second.times[k - 1] : -1;
        const std::pair<std::int64_t, std::int64_t> key{kth, c->second.times.front()};
        if (victim == cached.end() || key < worst) {
          worst = key;
          victim = c;
        }
      }
      cached.erase(victim);
    }
    Item fresh;
    record(fresh, clock);
    cached[r.obj_id] = fresh;
  }
  return hits;
}

// Segmented LRU with equal segments.
std::uint64_t refSlru(const std::vector<Request>& trace, std::size_t capacity,
                      std::size_t segments) {
  std::vector<std::list<std::uint64_t>> levels(segments);  // front = MRU
  const std::size_t budget = capacity / segments;
  std::uint64_t hits = 0;
  const auto rebalance = [&] {
    for (std::size_t level = segments; level-- > 1;) {
      while (levels[level].size() > budget) {
        const std::uint64_t demoted = levels[level].back();
        levels[level].pop_back();
        levels[level - 1].push_front(demoted);
      }
    }
  };
  const auto total = [&] {
    std::size_t n = 0;
    for (const auto& level : levels) n += level.size();
    return n;
  };
  for (const Request& r : trace) {
    std::size_t found_at = segments;
    std::list<std::uint64_t>::iterator found;
    for (std::size_t level = 0; level < segments; ++level) {
      const auto it = std::find(levels[level].begin(), levels[level].end(), r.obj_id);
      if (it != levels[level].end()) {
        found_at = level;
        found = it;
        break;
      }
    }
    if (found_at < segments) {
      ++hits;
      if (found_at + 1 == segments) {
        levels[found_at].erase(found);
        levels[found_at].push_front(r.obj_id);
      } else {
        levels[found_at].erase(found);
        levels[found_at + 1].push_front(r.obj_id);
        rebalance();
      }
      continue;
    }
    while (total() >= capacity) {
      for (std::size_t level = 0; level < segments; ++level) {
        if (!levels[level].empty()) {
          levels[level].pop_back();
          break;
        }
      }
    }
    levels[0].push_front(r.obj_id);
    rebalance();
  }
  return hits;
}

// ARC's reference is the longest of these, so its definition sits at the
// bottom of the file; this is the declaration the tests use.
std::uint64_t refArc(const std::vector<Request>& trace, std::size_t c);

// ---------------------------------------------------------------- the tests

void testReferenceCrossValidation() {
  for (const std::uint64_t seed : {1ULL, 7ULL, 42ULL}) {
    for (const std::size_t capacity : {3u, 10u, 64u, 200u}) {
      const auto trace = makeTrace(3000, 300, seed);
      const std::string label =
          " seed=" + std::to_string(seed) + " cap=" + std::to_string(capacity);

      TEST_CASE("LRU matches the reference" + label) {
        CHECK_EQ(run("LRU", trace, capacity).hits, refLru(trace, capacity));
      }
      TEST_CASE("FIFO matches the reference" + label) {
        CHECK_EQ(run("FIFO", trace, capacity).hits, refFifo(trace, capacity));
      }
      TEST_CASE("MRU matches the reference" + label) {
        CHECK_EQ(run("MRU", trace, capacity).hits, refMru(trace, capacity));
      }
      TEST_CASE("LFU matches the reference" + label) {
        CHECK_EQ(run("LFU", trace, capacity).hits, refLfu(trace, capacity));
      }
      TEST_CASE("LFU-DA matches the reference" + label) {
        CHECK_EQ(run("LFU-DA", trace, capacity).hits, refLfuDa(trace, capacity));
      }
      TEST_CASE("Clock matches the reference" + label) {
        CHECK_EQ(run("Clock", trace, capacity).hits, refClock(trace, capacity));
      }
      TEST_CASE("Sieve matches the reference" + label) {
        CHECK_EQ(run("Sieve", trace, capacity).hits, refSieve(trace, capacity));
      }
      TEST_CASE("ARC matches the FAST'03 pseudocode" + label) {
        CHECK_EQ(run("ARC", trace, capacity).hits, refArc(trace, capacity));
      }
      TEST_CASE("2Q matches the reference" + label) {
        CHECK_EQ(run("2Q", trace, capacity).hits, refTwoQ(trace, capacity, 0.25, 0.5));
      }
      TEST_CASE("LRU-1 matches the reference" + label) {
        CHECK_EQ(run("LRU-K", trace, capacity, "k=1").hits, refLruK(trace, capacity, 1));
      }
      TEST_CASE("LRU-2 matches the reference" + label) {
        CHECK_EQ(run("LRU-K", trace, capacity, "k=2").hits, refLruK(trace, capacity, 2));
      }
      TEST_CASE("LRU-3 matches the reference" + label) {
        CHECK_EQ(run("LRU-K", trace, capacity, "k=3").hits, refLruK(trace, capacity, 3));
      }
      TEST_CASE("SLRU-4 matches the reference" + label) {
        CHECK_EQ(run("SLRU", trace, capacity, "segments=4").hits, refSlru(trace, capacity, 4));
      }
      TEST_CASE("Belady matches its definition" + label) {
        CHECK_EQ(run("Belady", trace, capacity).hits, refBelady(trace, capacity));
      }
    }
  }
}

void testUniversalInvariants() {
  const auto policies = PolicyRegistry::instance().names();
  const auto uniform = makeTrace(20000, 2000, 5);
  const auto variable = makeTrace(20000, 2000, 6, /*variable_sizes=*/true);

  for (const std::string& policy : policies) {
    TEST_CASE(policy + ": uniform sizes, capacity respected") {
      const RunResult result = run(policy, uniform, 256);
      CHECK(result.occupied <= 256);
      CHECK(result.resident <= 256);
      // Something must have been cached, and something must have been
      // evicted: a policy that admits nothing or evicts nothing has a bug
      // that a hit-ratio check alone can hide.
      CHECK(result.resident > 0);
      CHECK(result.evictions > 0);
      CHECK(result.hits > 0);
    }

    TEST_CASE(policy + ": variable sizes, capacity respected") {
      const RunResult result = run(policy, variable, 8192);
      CHECK(result.occupied <= 8192);
      CHECK(result.hits > 0);
    }

    TEST_CASE(policy + ": tiny capacity does not deadlock or overflow") {
      // Capacity 1 is the case where every eviction path runs on an almost
      // empty structure, and where an off-by-one in a budget split means an
      // infinite loop rather than a wrong number.
      const RunResult result = run(policy, uniform, 1);
      CHECK(result.occupied <= 1);
      CHECK(result.resident <= 1);
    }

    TEST_CASE(policy + ": capacity larger than the working set never evicts") {
      const auto small = makeTrace(5000, 100, 9);
      const RunResult result = run(policy, small, 4096);
      CHECK_EQ(result.evictions, std::uint64_t{0});
      CHECK_EQ(result.resident, std::size_t{100});
      // Every object missed exactly once, so hits are requests minus
      // distinct objects.
      CHECK_EQ(result.hits, std::uint64_t{5000 - 100});
    }
  }
}

void testKnownRelationships() {
  const auto trace = makeTrace(30000, 3000, 13);

  TEST_CASE("Belady is an upper bound on every online policy") {
    const std::uint64_t optimal = run("Belady", trace, 300).hits;
    for (const std::string& policy : PolicyRegistry::instance().names()) {
      const std::uint64_t hits = run(policy, trace, 300).hits;
      if (hits > optimal) {
        std::printf("\n      %s beat Belady: %llu > %llu\n", policy.c_str(),
                    (unsigned long long)hits, (unsigned long long)optimal);
      }
      CHECK(hits <= optimal);
    }
  }

  TEST_CASE("LRU-1 is exactly LRU") {
    CHECK_EQ(run("LRU-K", trace, 300, "k=1").hits, run("LRU", trace, 300).hits);
  }

  TEST_CASE("SLRU with one segment is exactly LRU") {
    CHECK_EQ(run("SLRU", trace, 300, "segments=1").hits, run("LRU", trace, 300).hits);
  }

  TEST_CASE("Clock with a 1-bit counter beats FIFO on a skewed trace") {
    // Not a law of nature, but it holds on any trace with frequency skew,
    // and a Clock that failed to use its reference bit would fall to FIFO's
    // number exactly.
    CHECK(run("Clock", trace, 300).hits > run("FIFO", trace, 300).hits);
  }

  TEST_CASE("scan resistance: the scan-resistant policies beat LRU") {
    // 50 hot objects passed over four times, then a 100-object scan, in a
    // cache of 100. LRU is flushed by every scan and has to re-fault the
    // whole hot set each loop; a scan-resistant policy keeps it.
    const auto scan = makeScanTrace(40, 50, 4, 100);
    const std::uint64_t lru = run("LRU", scan, 100).hits;
    for (const char* policy : {"2Q", "ARC", "S3FIFO", "SLRU", "LRU-K", "W-TinyLFU"}) {
      const std::uint64_t hits = run(policy, scan, 100).hits;
      if (hits <= lru) {
        std::printf("\n      %s did not beat LRU on the scan trace: %llu <= %llu\n", policy,
                    (unsigned long long)hits, (unsigned long long)lru);
      }
      CHECK(hits > lru);
    }
  }

  TEST_CASE("scan resistance: ghost-list policies need a ghost list long enough") {
    // The same trace with a longer scan. 2Q's resistance comes entirely from
    // A1out, so a scan longer than A1out ages the working set's ids out of
    // it and 2Q falls back to exactly LRU's behaviour. This is a real
    // property of the algorithm rather than a defect, and it is worth
    // pinning down: it is the difference between 2Q and the policies whose
    // evidence lives on the cached objects themselves (ARC's T2, S3FIFO's
    // main queue, W-TinyLFU's sketch), which keep working.
    const auto long_scan = makeScanTrace(40, 50, 4, 400);
    const std::uint64_t lru = run("LRU", long_scan, 100).hits;
    CHECK_EQ(run("2Q", long_scan, 100).hits, lru);
    for (const char* policy : {"ARC", "S3FIFO", "SLRU", "LRU-K", "W-TinyLFU"}) {
      CHECK(run(policy, long_scan, 100).hits > lru);
    }
  }

  TEST_CASE("every policy beats a capacity-1 cache on a looping trace") {
    std::vector<Request> loop;
    for (int round = 0; round < 200; ++round) {
      for (std::uint64_t i = 0; i < 50; ++i) {
        Request r;
        r.obj_id = i;
        r.size = 1;
        loop.push_back(r);
      }
    }
    for (const std::string& policy : PolicyRegistry::instance().names()) {
      if (policy == "Belady") continue;  // this trace has no oracle column
      // Capacity 50 holds the whole loop, so after the first pass everything
      // hits.
      const RunResult result = run(policy, loop, 50);
      CHECK_EQ(result.hits, std::uint64_t{200 * 50 - 50});
    }
  }
}

void testPolicyParameters() {
  const auto trace = makeTrace(20000, 2000, 21);

  TEST_CASE("policy names are reported with their distinguishing parameters") {
    PolicyConfig pc;
    pc.capacity_bytes = 100;
    pc.entry_hint = 100;
    pc.params = ParamMap("k=4");
    CHECK_EQ(PolicyRegistry::instance().create("LRU-K", pc)->name(), std::string("LRU-4"));
    pc.params = ParamMap("segments=2");
    CHECK_EQ(PolicyRegistry::instance().create("SLRU", pc)->name(), std::string("SLRU-2"));
    pc.params = ParamMap("n-bit-counter=3");
    CHECK_EQ(PolicyRegistry::instance().create("Clock", pc)->name(), std::string("Clock-3"));
  }

  TEST_CASE("policy names and aliases resolve case- and separator-insensitively") {
    PolicyConfig pc;
    pc.capacity_bytes = 10;
    pc.entry_hint = 10;
    for (const char* spelling : {"s3fifo", "S3FIFO", "S3-FIFO", "s3_fifo"}) {
      CHECK_EQ(PolicyRegistry::instance().create(spelling, pc)->name(), std::string("S3FIFO"));
    }
    CHECK_EQ(PolicyRegistry::instance().create("opt", pc)->name(), std::string("Belady"));
    CHECK_EQ(PolicyRegistry::instance().create("lruk", pc)->name(), std::string("LRU-2"));
  }

  TEST_CASE("an unknown policy name is rejected with the available list") {
    PolicyConfig pc;
    bool threw = false;
    try {
      (void)PolicyRegistry::instance().create("not-a-policy", pc);
    } catch (const ConfigError& error) {
      threw = true;
      CHECK(std::string(error.what()).find("LRU") != std::string::npos);
    }
    CHECK(threw);
  }

  TEST_CASE("S3FIFO's small queue ratio changes behaviour monotonically") {
    // A larger small queue means more of the cache is spent on unproven
    // objects, so on a skewed trace the hit ratio should not improve as the
    // ratio grows towards 1.
    const std::uint64_t tenth = run("S3FIFO", trace, 200, "small-size-ratio=0.1").hits;
    const std::uint64_t ninety = run("S3FIFO", trace, 200, "small-size-ratio=0.9").hits;
    CHECK(tenth >= ninety);
  }

  TEST_CASE("Belady on a trace without an oracle column fails loudly") {
    std::vector<Request> no_oracle;
    for (std::uint64_t i = 0; i < 100; ++i) {
      Request r;
      r.obj_id = i % 10;
      r.size = 1;
      r.next_access_vtime = kNoOracle;
      no_oracle.push_back(r);
    }
    bool threw = false;
    try {
      run("Belady", no_oracle, 5);
    } catch (const ConfigError& error) {
      threw = true;
      CHECK(std::string(error.what()).find("oracle") != std::string::npos);
    }
    CHECK(threw);
  }
}

void testArcInternals() {
  TEST_CASE("ARC: p stays within [0, capacity] and lists stay bounded") {
    const auto trace = makeTrace(30000, 3000, 31);
    const std::uint64_t capacity = 300;
    PolicyConfig pc;
    pc.capacity_bytes = capacity;
    pc.entry_hint = static_cast<std::size_t>(capacity);
    auto policy = std::make_unique<policies::Arc>(pc);
    policies::Arc* arc = policy.get();
    CacheOptions options;
    options.capacity_bytes = capacity;
    options.entry_hint = static_cast<std::size_t>(capacity);
    Cache cache(options, std::move(policy));
    for (const Request& r : trace) {
      cache.access(r);
      CHECK(arc->target() <= static_cast<double>(capacity));
      // |T1| + |T2| is the cache itself, and the directory is bounded at 2c.
      CHECK(arc->t1Count() + arc->t2Count() <= capacity);
      CHECK(arc->t1Count() + arc->b1Count() <= capacity + 1);
      CHECK(arc->t1Count() + arc->t2Count() + arc->b1Count() + arc->b2Count() <=
            2 * capacity + 1);
    }
    // p must actually have moved, or the adaptation is dead code.
    CHECK(arc->target() > 0.0);
  }
}

// A direct transcription of the FAST'03 paper's pseudocode in object counts,
// which is what the byte-based implementation must reduce to when every
// object is size 1.
std::uint64_t refArc(const std::vector<Request>& trace, std::size_t c) {
  std::list<std::uint64_t> t1, t2, b1, b2;  // front = MRU
  // Real-valued, as in the paper: the adaptation step is a ratio of ghost
  // list sizes and is almost never whole.
  double p = 0.0;
  std::uint64_t hits = 0;
  const auto find = [](std::list<std::uint64_t>& l, std::uint64_t id) {
    return std::find(l.begin(), l.end(), id);
  };
  const auto replace = [&](bool request_in_b2) {
    const auto t1_size = static_cast<double>(t1.size());
    if (!t1.empty() && (t1_size > p || (request_in_b2 && t1_size == p))) {
      b1.push_front(t1.back());
      t1.pop_back();
    } else if (!t2.empty()) {
      b2.push_front(t2.back());
      t2.pop_back();
    } else if (!t1.empty()) {
      b1.push_front(t1.back());
      t1.pop_back();
    }
  };
  for (const Request& r : trace) {
    const std::uint64_t id = r.obj_id;
    const auto in_t1 = find(t1, id);
    if (in_t1 != t1.end()) {
      ++hits;
      t1.erase(in_t1);
      t2.push_front(id);
      continue;
    }
    const auto in_t2 = find(t2, id);
    if (in_t2 != t2.end()) {
      ++hits;
      t2.erase(in_t2);
      t2.push_front(id);
      continue;
    }
    const auto in_b1 = find(b1, id);
    if (in_b1 != b1.end()) {
      const double delta = std::max(
          static_cast<double>(b2.size()) / static_cast<double>(std::max<std::size_t>(b1.size(), 1)),
          1.0);
      p = std::min(static_cast<double>(c), p + delta);
      if (t1.size() + t2.size() >= c) replace(false);
      b1.erase(in_b1);
      t2.push_front(id);
      continue;
    }
    const auto in_b2 = find(b2, id);
    if (in_b2 != b2.end()) {
      const double delta = std::max(
          static_cast<double>(b1.size()) / static_cast<double>(std::max<std::size_t>(b2.size(), 1)),
          1.0);
      p = std::max(p - delta, 0.0);
      if (t1.size() + t2.size() >= c) replace(true);
      b2.erase(in_b2);
      t2.push_front(id);
      continue;
    }
    if (t1.size() + b1.size() >= c) {
      if (!b1.empty()) {
        b1.pop_back();
        if (t1.size() + t2.size() >= c) replace(false);
      } else if (!t1.empty()) {
        t1.pop_back();
      }
    } else if (t1.size() + t2.size() + b1.size() + b2.size() >= c) {
      if (t1.size() + t2.size() + b1.size() + b2.size() >= 2 * c && !b2.empty()) {
        b2.pop_back();
      }
      if (t1.size() + t2.size() >= c) replace(false);
    }
    t1.push_front(id);
  }
  return hits;
}

}  // namespace

int main() {
  std::printf("Reference cross-validation\n");
  testReferenceCrossValidation();
  std::printf("Universal invariants\n");
  testUniversalInvariants();
  std::printf("Known relationships\n");
  testKnownRelationships();
  std::printf("Policy parameters\n");
  testPolicyParameters();
  std::printf("ARC internals\n");
  testArcInternals();
  return cachesim::test::summarize("policy_test");
}
