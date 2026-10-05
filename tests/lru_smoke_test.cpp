#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>

#include "internal/cache/cache.hpp"
#include "policies/lru_policy.hpp"

// Deliberately not using assert(): NDEBUG (Release builds) compiles it out,
// and these checks must always run.
#define CHECK(cond)                                                  \
  do {                                                                \
    if (!(cond)) {                                                    \
      std::fprintf(stderr, "FAILED: %s (line %d)\n", #cond, __LINE__); \
      std::exit(1);                                                   \
    }                                                                  \
  } while (0)

using cachesim::Cache;
using cachesim::LRUPolicy;
using cachesim::Request;

namespace {

void testBasicHitMiss() {
  Cache cache(/*capacity_bytes=*/1024, std::make_unique<LRUPolicy>(), /*max_entries=*/16);

  CHECK(cache.access(Request{1, 100, 0}) == false);  // first touch: miss
  CHECK(cache.access(Request{1, 100, 1}) == true);   // second touch: hit
  CHECK(cache.occupiedBytes() == 100);
  CHECK(cache.objectCount() == 1);
}

void testLruEvictsLeastRecentlyUsed() {
  // Capacity fits exactly 3 unit-size objects.
  Cache cache(/*capacity_bytes=*/3, std::make_unique<LRUPolicy>(), /*max_entries=*/8);

  CHECK(cache.access(Request{1, 1, 0}) == false);
  CHECK(cache.access(Request{2, 1, 1}) == false);
  CHECK(cache.access(Request{3, 1, 2}) == false);
  CHECK(cache.objectCount() == 3);

  // Touch 1 so it becomes most-recently-used; 2 is now the LRU victim.
  CHECK(cache.access(Request{1, 1, 3}) == true);

  // Inserting 4 must evict 2 (the least recently used), not 1 or 3.
  CHECK(cache.access(Request{4, 1, 4}) == false);
  CHECK(cache.objectCount() == 3);

  // Check survivors first: querying an evicted key below is itself a new
  // miss that triggers another eviction, so it must come last.
  CHECK(cache.access(Request{3, 1, 5}) == true);  // 3 survived: hit
  CHECK(cache.access(Request{1, 1, 6}) == true);  // 1 survived: hit
  CHECK(cache.access(Request{2, 1, 7}) == false);  // 2 was evicted: miss
}

void testOversizedObjectNeverAdmitted() {
  Cache cache(/*capacity_bytes=*/10, std::make_unique<LRUPolicy>(), /*max_entries=*/4);

  CHECK(cache.access(Request{1, 20, 0}) == false);
  CHECK(cache.occupiedBytes() == 0);
  CHECK(cache.objectCount() == 0);
}

void testMaxEntriesExhaustionThrows() {
  // CacheStructure reserves 1.5x max_entries as a safety margin (see
  // cache_structure.cpp), so max_entries=2 actually allows 3 concurrent
  // objects before the arena is truly exhausted.
  Cache cache(/*capacity_bytes=*/1000, std::make_unique<LRUPolicy>(), /*max_entries=*/2);

  CHECK(cache.access(Request{1, 1, 0}) == false);
  CHECK(cache.access(Request{2, 1, 1}) == false);
  CHECK(cache.access(Request{3, 1, 2}) == false);

  bool threw = false;
  try {
    cache.access(Request{4, 1, 3});  // a 4th distinct object exceeds the reserved capacity
  } catch (const std::runtime_error&) {
    threw = true;
  }
  CHECK(threw);
}

}  // namespace

int main() {
  testBasicHitMiss();
  testLruEvictsLeastRecentlyUsed();
  testOversizedObjectNeverAdmitted();
  testMaxEntriesExhaustionThrows();

  std::printf("All tests passed\n");
  return 0;
}
