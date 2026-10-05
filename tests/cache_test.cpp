// Tests for the Cache engine itself: the hit/miss decision, byte accounting,
// eviction sequencing, size changes, oversized objects and the observer hook.
//
// These are the invariants every policy relies on, so a break here would
// corrupt every result rather than one row of a sweep.

#include <memory>
#include <stdexcept>
#include <vector>

#include "internal/cache/cache.hpp"
#include "internal/common/error.hpp"
#include "internal/common/hash.hpp"
#include "internal/datastructures/queue.hpp"
#include "internal/eviction/policy_registry.hpp"
#include "policies/lru.hpp"
#include "tests/test_support.hpp"

using namespace cachesim;

namespace {

Request req(std::uint64_t obj_id, std::uint32_t size = 1, std::int64_t time = 0) {
  Request r;
  r.obj_id = obj_id;
  r.size = size;
  r.clock_time = time;
  return r;
}

std::unique_ptr<Cache> makeCache(std::uint64_t capacity, const char* policy = "LRU",
                                 CacheOptions overrides = {}) {
  PolicyConfig pc;
  pc.capacity_bytes = capacity;
  pc.entry_hint = static_cast<std::size_t>(capacity);
  CacheOptions options = overrides;
  options.capacity_bytes = capacity;
  if (options.entry_hint == 0) options.entry_hint = static_cast<std::size_t>(capacity);
  return std::make_unique<Cache>(options, PolicyRegistry::instance().create(policy, pc));
}

// A policy that tracks nothing, to prove the engine reports a broken policy
// rather than corrupting itself or looping.
class BrokenPolicy final : public IEvictionPolicy {
 public:
  [[nodiscard]] std::string name() const override { return "Broken"; }
  Status onAdmit(internal::CacheEntry*, const Request&) override { return Status::kSuccess; }
  Status onHit(internal::CacheEntry*, const Request&) override { return Status::kSuccess; }
  internal::CacheEntry* evict() override { return nullptr; }
  Status onRemove(internal::CacheEntry*) override { return Status::kSuccess; }
};

class RecordingObserver final : public ICacheObserver {
 public:
  void onEvicted(std::uint64_t obj_id, std::uint32_t size) override {
    evicted.push_back({obj_id, size});
  }
  struct Eviction {
    std::uint64_t obj_id;
    std::uint32_t size;
  };
  std::vector<Eviction> evicted;
};

void testBasics() {
  TEST_CASE("Cache: a cold cache misses, a warm one hits") {
    auto cache = makeCache(10);
    CHECK(!cache->access(req(1)));
    CHECK(cache->access(req(1)));
    CHECK_EQ(cache->stats().n_req, std::uint64_t{2});
    CHECK_EQ(cache->stats().n_hit, std::uint64_t{1});
    CHECK_EQ(cache->stats().nMiss(), std::uint64_t{1});
    CHECK_NEAR(cache->stats().missRatio(), 0.5, 1e-12);
  }

  TEST_CASE("Cache: occupancy never exceeds capacity") {
    auto cache = makeCache(1000);
    Rng rng(11);
    for (std::uint64_t i = 0; i < 20000; ++i) {
      const auto size = static_cast<std::uint32_t>(1 + rng.below(200));
      cache->access(req(rng.below(5000), size));
      CHECK(cache->occupiedBytes() <= cache->capacityBytes());
    }
  }

  TEST_CASE("Cache: byte counters follow request sizes") {
    auto cache = makeCache(1000);
    cache->access(req(1, 100));
    cache->access(req(2, 250));
    cache->access(req(1, 100));
    CHECK_EQ(cache->stats().n_req_byte, std::uint64_t{450});
    CHECK_EQ(cache->stats().n_hit_byte, std::uint64_t{100});
    CHECK_EQ(cache->stats().nMissByte(), std::uint64_t{350});
    CHECK_NEAR(cache->stats().byteMissRatio(), 350.0 / 450.0, 1e-12);
  }

  TEST_CASE("Cache: admit and evict counts balance against residency") {
    auto cache = makeCache(50);
    for (std::uint64_t i = 0; i < 500; ++i) cache->access(req(i));
    const Stats& stats = cache->stats();
    CHECK_EQ(stats.n_admit, std::uint64_t{500});
    CHECK_EQ(stats.n_admit - stats.n_evict, static_cast<std::uint64_t>(cache->objectCount()));
    CHECK_EQ(cache->objectCount(), std::size_t{50});
  }

  TEST_CASE("Cache: an object larger than the cache is never admitted") {
    auto cache = makeCache(100);
    CHECK(!cache->access(req(1, 500)));
    CHECK_EQ(cache->objectCount(), std::size_t{0});
    CHECK_EQ(cache->stats().n_oversized, std::uint64_t{1});
    // And it does not disturb what is already cached.
    cache->access(req(2, 50));
    CHECK(!cache->access(req(1, 500)));
    CHECK(cache->access(req(2, 50)));
    CHECK_EQ(cache->stats().n_oversized, std::uint64_t{2});
  }

  TEST_CASE("Cache: one large miss can evict several objects") {
    auto cache = makeCache(100);
    for (std::uint64_t i = 0; i < 10; ++i) cache->access(req(i, 10));
    CHECK_EQ(cache->objectCount(), std::size_t{10});
    cache->access(req(100, 100));
    CHECK_EQ(cache->objectCount(), std::size_t{1});
    CHECK_EQ(cache->occupiedBytes(), std::uint64_t{100});
    CHECK_EQ(cache->stats().n_evict, std::uint64_t{10});
  }
}

void testSizeChanges() {
  TEST_CASE("Cache: a hit at a new size updates occupancy") {
    auto cache = makeCache(1000);
    cache->access(req(1, 100));
    CHECK_EQ(cache->occupiedBytes(), std::uint64_t{100});
    cache->access(req(1, 400));
    CHECK_EQ(cache->occupiedBytes(), std::uint64_t{400});
    cache->access(req(1, 50));
    CHECK_EQ(cache->occupiedBytes(), std::uint64_t{50});
    CHECK_EQ(cache->stats().n_hit, std::uint64_t{2});
  }

  TEST_CASE("Cache: growing an object can evict others") {
    auto cache = makeCache(100);
    cache->access(req(1, 10));
    cache->access(req(2, 10));
    cache->access(req(3, 10));
    cache->access(req(1, 90));  // hit, but now needs 90 of the 100 bytes
    CHECK(cache->occupiedBytes() <= 100);
    CHECK(cache->objectCount() <= 2);
  }

  TEST_CASE("Cache: an object that grows past capacity is dropped") {
    auto cache = makeCache(100);
    cache->access(req(1, 10));
    CHECK(cache->access(req(1, 500)));  // still reported as a hit: it was there
    CHECK_EQ(cache->objectCount(), std::size_t{0});
    CHECK_EQ(cache->occupiedBytes(), std::uint64_t{0});
    // And the cache is still usable.
    CHECK(!cache->access(req(2, 10)));
    CHECK(cache->access(req(2, 10)));
  }

  TEST_CASE("Cache: update_size_on_hit=false pins the first size seen") {
    CacheOptions options;
    options.update_size_on_hit = false;
    auto cache = makeCache(1000, "LRU", options);
    cache->access(req(1, 100));
    cache->access(req(1, 900));
    CHECK_EQ(cache->occupiedBytes(), std::uint64_t{100});
    // The request's own size still counts towards traffic, which is what the
    // byte miss ratio is about.
    CHECK_EQ(cache->stats().n_req_byte, std::uint64_t{1000});
  }

  TEST_CASE("Cache: per-object metadata is charged to occupancy only") {
    CacheOptions options;
    options.obj_metadata_size = 20;
    auto cache = makeCache(1000, "LRU", options);
    cache->access(req(1, 100));
    CHECK_EQ(cache->occupiedBytes(), std::uint64_t{120});
    CHECK_EQ(cache->stats().n_req_byte, std::uint64_t{100});
    // A repeat at the same size must not be mistaken for a size change.
    cache->access(req(1, 100));
    CHECK_EQ(cache->occupiedBytes(), std::uint64_t{120});
    CHECK_EQ(cache->objectCount(), std::size_t{1});
  }
}

void testRemovalAndClear() {
  TEST_CASE("Cache: remove drops a cached object") {
    auto cache = makeCache(100);
    cache->access(req(1, 10));
    cache->access(req(2, 10));
    CHECK(cache->remove(1));
    CHECK(!cache->remove(1));
    CHECK(!cache->remove(999));
    CHECK_EQ(cache->objectCount(), std::size_t{1});
    CHECK_EQ(cache->occupiedBytes(), std::uint64_t{10});
    CHECK(!cache->access(req(1, 10)));  // now a miss
  }

  TEST_CASE("Cache: clear empties contents but keeps counters") {
    auto cache = makeCache(100);
    for (std::uint64_t i = 0; i < 50; ++i) cache->access(req(i));
    const std::uint64_t requests = cache->stats().n_req;
    cache->clear();
    CHECK_EQ(cache->objectCount(), std::size_t{0});
    CHECK_EQ(cache->occupiedBytes(), std::uint64_t{0});
    CHECK_EQ(cache->stats().n_req, requests);
    // Usable afterwards, and nothing survived.
    CHECK(!cache->access(req(0)));
  }

  TEST_CASE("Cache: resetStats keeps contents but zeroes counters") {
    auto cache = makeCache(100);
    for (std::uint64_t i = 0; i < 20; ++i) cache->access(req(i));
    cache->resetStats();
    CHECK_EQ(cache->stats().n_req, std::uint64_t{0});
    CHECK_EQ(cache->objectCount(), std::size_t{20});
    // This is the warm-up discard: the cache stays warm, the numbers do not.
    CHECK(cache->access(req(5)));
    CHECK_EQ(cache->stats().n_hit, std::uint64_t{1});
  }

  TEST_CASE("Cache: clear then refill works for every policy") {
    for (const std::string& name : PolicyRegistry::instance().names()) {
      if (name == "Belady") continue;  // needs an oracle trace
      auto cache = makeCache(32, name.c_str());
      for (std::uint64_t i = 0; i < 200; ++i) cache->access(req(i % 64));
      cache->clear();
      CHECK_EQ(cache->objectCount(), std::size_t{0});
      for (std::uint64_t i = 0; i < 200; ++i) cache->access(req(i % 64));
      CHECK(cache->occupiedBytes() <= cache->capacityBytes());
    }
  }
}

void testObserverAndErrors() {
  TEST_CASE("Cache: observers see every eviction, with the right size") {
    auto cache = makeCache(30);
    RecordingObserver observer;
    cache->addObserver(&observer);
    for (std::uint64_t i = 0; i < 5; ++i) cache->access(req(i, 10));
    // Capacity 30 with 10-byte objects: ids 0 and 1 should have gone, in LRU
    // order, which for an insert-only workload is insertion order.
    CHECK_EQ(observer.evicted.size(), std::size_t{2});
    CHECK_EQ(observer.evicted[0].obj_id, std::uint64_t{0});
    CHECK_EQ(observer.evicted[0].size, std::uint32_t{10});
    CHECK_EQ(observer.evicted[1].obj_id, std::uint64_t{1});
  }

  TEST_CASE("Cache: an explicit remove notifies observers too") {
    auto cache = makeCache(100);
    RecordingObserver observer;
    cache->addObserver(&observer);
    cache->access(req(7, 10));
    CHECK(cache->remove(7));
    CHECK_EQ(observer.evicted.size(), std::size_t{1});
    CHECK_EQ(observer.evicted[0].obj_id, std::uint64_t{7});
  }

  TEST_CASE("Cache: a policy with no victim raises a diagnosable error") {
    CacheOptions options;
    options.capacity_bytes = 2;
    options.entry_hint = 4;
    Cache cache(options, std::make_unique<BrokenPolicy>());
    cache.access(req(1));
    cache.access(req(2));
    bool threw = false;
    try {
      cache.access(req(3));  // needs an eviction the policy cannot supply
    } catch (const EngineError& error) {
      threw = true;
      // The message has to name the policy and the state, or it is useless
      // eight hours into a sweep.
      const std::string what = error.what();
      CHECK(what.find("Broken") != std::string::npos);
      CHECK(what.find("evict") != std::string::npos);
    }
    CHECK(threw);
  }

  TEST_CASE("Cache: constructing without a policy is an error") {
    bool threw = false;
    try {
      CacheOptions options;
      options.capacity_bytes = 10;
      Cache cache(options, nullptr);
      (void)cache;
    } catch (const EngineError&) {
      threw = true;
    }
    CHECK(threw);
  }
}

void testZeroAndEdgeCases() {
  TEST_CASE("Cache: capacity of one object") {
    auto cache = makeCache(1);
    CHECK(!cache->access(req(1)));
    CHECK(cache->access(req(1)));
    CHECK(!cache->access(req(2)));
    CHECK(!cache->access(req(1)));
    CHECK_EQ(cache->objectCount(), std::size_t{1});
  }

  TEST_CASE("Cache: a zero-size object is admitted and occupies nothing") {
    auto cache = makeCache(10);
    CHECK(!cache->access(req(1, 0)));
    CHECK(cache->access(req(1, 0)));
    CHECK_EQ(cache->occupiedBytes(), std::uint64_t{0});
    // The byte miss ratio has no traffic to divide by and must not divide by
    // zero.
    CHECK_NEAR(cache->stats().byteMissRatio(), 0.0, 1e-12);
  }

  TEST_CASE("Cache: repeated access to one object never evicts") {
    auto cache = makeCache(10);
    for (int i = 0; i < 1000; ++i) cache->access(req(42));
    CHECK_EQ(cache->stats().n_evict, std::uint64_t{0});
    CHECK_EQ(cache->objectCount(), std::size_t{1});
    CHECK_EQ(cache->stats().n_hit, std::uint64_t{999});
  }

  TEST_CASE("Stats: summation and reset") {
    Stats a;
    a.n_req = 10;
    a.n_hit = 4;
    a.n_req_byte = 100;
    a.n_hit_byte = 40;
    Stats b;
    b.n_req = 5;
    b.n_hit = 5;
    b.n_req_byte = 50;
    b.n_hit_byte = 50;
    a += b;
    CHECK_EQ(a.n_req, std::uint64_t{15});
    CHECK_EQ(a.n_hit, std::uint64_t{9});
    CHECK_NEAR(a.hitRatio(), 9.0 / 15.0, 1e-12);
    a.reset();
    CHECK_EQ(a.n_req, std::uint64_t{0});
    CHECK_NEAR(a.missRatio(), 0.0, 1e-12);
  }
}

}  // namespace

int main() {
  std::printf("Cache: basics\n");
  testBasics();
  std::printf("Cache: size changes\n");
  testSizeChanges();
  std::printf("Cache: removal and clear\n");
  testRemovalAndClear();
  std::printf("Cache: observers and errors\n");
  testObserverAndErrors();
  std::printf("Cache: edge cases\n");
  testZeroAndEdgeCases();
  return cachesim::test::summarize("cache_test");
}
