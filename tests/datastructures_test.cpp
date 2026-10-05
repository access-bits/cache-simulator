// Unit tests for the intrusive data structures the policies are built from.
//
// These are tested directly rather than only through the policies, because a
// bug in one of them (a lost ghost entry, a heap that does not re-sift) shows
// up as a slightly wrong hit ratio rather than as a crash, and a slightly
// wrong hit ratio is exactly the kind of thing that survives all the way into
// a published number.

#include <algorithm>
#include <set>
#include <vector>

#include "internal/cache/cache_structure.hpp"
#include "internal/common/hash.hpp"
#include "internal/datastructures/frequency_sketch.hpp"
#include "internal/datastructures/id_history.hpp"
#include "internal/datastructures/priority_queue.hpp"
#include "internal/datastructures/queue.hpp"
#include "internal/datastructures/random_bag.hpp"
#include "tests/test_support.hpp"

using namespace cachesim;
using cachesim::internal::CacheEntry;
using cachesim::internal::CacheStructure;

namespace {

// Hands out real CacheEntry objects, because the structures work with
// CacheEntry* and its identity/size fields are only writable by
// CacheStructure.
class Arena {
 public:
  explicit Arena(std::size_t hint = 1024) : structure_(hint) {}

  CacheEntry* add(std::uint64_t obj_id, std::uint32_t size = 1) {
    return structure_.admit(obj_id, size);
  }

  CacheStructure& structure() { return structure_; }

 private:
  CacheStructure structure_;
};

void testCacheStructure() {
  TEST_CASE("CacheStructure: admit, find, erase, byte accounting") {
    CacheStructure structure(16);
    CacheEntry* a = structure.admit(1, 100);
    CacheEntry* b = structure.admit(2, 250);
    CHECK(a != nullptr);
    CHECK(b != nullptr);
    CHECK_EQ(structure.objectCount(), std::size_t{2});
    CHECK_EQ(structure.occupiedBytes(), std::uint64_t{350});
    CHECK_EQ(structure.find(1), a);
    CHECK_EQ(structure.find(2), b);
    CHECK(structure.find(3) == nullptr);
    CHECK(structure.contains(1));

    CHECK(ok(structure.erase(a)));
    CHECK_EQ(structure.objectCount(), std::size_t{1});
    CHECK_EQ(structure.occupiedBytes(), std::uint64_t{250});
    CHECK(structure.find(1) == nullptr);
  }

  TEST_CASE("CacheStructure: erase refuses an entry still tracked") {
    CacheStructure structure(16);
    Queue<> queue;
    CacheEntry* a = structure.admit(1, 10);
    CHECK(ok(queue.pushFront(a)));
    // The entry's metadata is still set, so erasing it would leave the queue
    // holding a node for a recycled slot.
    CHECK(!ok(structure.erase(a)));
    CHECK(ok(queue.unlink(a)));
    CHECK(ok(structure.erase(a)));
  }

  TEST_CASE("CacheStructure: freed slots are reused, not leaked") {
    CacheStructure structure(4);
    const std::size_t capacity_before = structure.arenaCapacity();
    for (std::uint64_t round = 0; round < 1000; ++round) {
      CacheEntry* entry = structure.admit(round, 1);
      CHECK(entry != nullptr);
      CHECK(ok(structure.erase(entry)));
    }
    // Every admit was followed by an erase, so one slot should have served
    // the whole run.
    CHECK_EQ(structure.arenaCapacity(), capacity_before);
    CHECK_EQ(structure.objectCount(), std::size_t{0});
  }

  TEST_CASE("CacheStructure: grows past the hint instead of failing") {
    // The hint is a sizing hint, not a cap: a trace with more small objects
    // than expected must keep running.
    CacheStructure structure(8);
    std::vector<CacheEntry*> entries;
    for (std::uint64_t i = 0; i < 5000; ++i) {
      CacheEntry* entry = structure.admit(i, 1);
      CHECK(entry != nullptr);
      entries.push_back(entry);
    }
    CHECK_EQ(structure.objectCount(), std::size_t{5000});
    CHECK(structure.blockCount() > 1);
    // Addresses handed out before the growth must still be valid and still
    // resolve to the same objects.
    for (std::uint64_t i = 0; i < 5000; ++i) {
      CHECK_EQ(structure.find(i), entries[i]);
      CHECK_EQ(entries[i]->objId(), i);
    }
  }

  TEST_CASE("CacheStructure: a hard max_entries cap is honoured") {
    CacheStructure structure(4, 10);
    for (std::uint64_t i = 0; i < 10; ++i) CHECK(structure.admit(i, 1) != nullptr);
    CHECK(structure.admit(99, 1) == nullptr);
  }

  TEST_CASE("CacheStructure: resize moves only the byte total") {
    CacheStructure structure(4);
    CacheEntry* a = structure.admit(1, 100);
    CHECK(ok(structure.resize(a, 40)));
    CHECK_EQ(structure.occupiedBytes(), std::uint64_t{40});
    CHECK_EQ(a->size(), std::uint32_t{40});
    CHECK_EQ(structure.find(1), a);
  }

  TEST_CASE("CacheStructure: clear empties the index and recycles slots") {
    CacheStructure structure(16);
    for (std::uint64_t i = 0; i < 16; ++i) structure.admit(i, 2);
    const std::size_t capacity = structure.arenaCapacity();
    structure.clear();
    CHECK_EQ(structure.objectCount(), std::size_t{0});
    CHECK_EQ(structure.occupiedBytes(), std::uint64_t{0});
    for (std::uint64_t i = 0; i < 16; ++i) CHECK(structure.admit(i, 2) != nullptr);
    CHECK_EQ(structure.arenaCapacity(), capacity);
  }
}

void testQueue() {
  TEST_CASE("Queue: front/back ordering and size") {
    Arena arena;
    Queue<> queue(8);
    CHECK(queue.empty());
    CacheEntry* a = arena.add(1);
    CacheEntry* b = arena.add(2);
    CacheEntry* c = arena.add(3);
    CHECK(ok(queue.pushFront(a)));
    CHECK(ok(queue.pushFront(b)));
    CHECK(ok(queue.pushBack(c)));
    // Order is now b, a, c.
    CHECK_EQ(queue.front(), b);
    CHECK_EQ(queue.back(), c);
    CHECK_EQ(queue.size(), std::size_t{3});
    CHECK_EQ(queue.next(b), a);
    CHECK_EQ(queue.next(a), c);
    CHECK(queue.next(c) == nullptr);
    CHECK_EQ(queue.prev(c), a);
    CHECK(queue.prev(b) == nullptr);
  }

  TEST_CASE("Queue: pushing an already-tracked entry fails") {
    Arena arena;
    Queue<> first(4);
    Queue<> second(4);
    CacheEntry* a = arena.add(1);
    CHECK(ok(first.pushFront(a)));
    CHECK(!ok(first.pushFront(a)));
    // And a second queue must not steal it, which would orphan the node the
    // first queue is still holding.
    CHECK(!ok(second.pushFront(a)));
    CHECK(first.contains(a));
    CHECK(!second.contains(a));
  }

  TEST_CASE("Queue: operations on an entry in a different queue fail") {
    Arena arena;
    Queue<> first(4);
    Queue<> second(4);
    CacheEntry* a = arena.add(1);
    CHECK(ok(first.pushFront(a)));
    CHECK(!ok(second.moveToFront(a)));
    CHECK(!ok(second.unlink(a)));
    CHECK(!ok(second.moveToBack(a)));
  }

  TEST_CASE("Queue: ownership check is sound across structure types") {
    // The point of the shared MetadataNode header: a PriorityQueue node and a
    // Queue node are different types, and asking a Queue whether it owns a
    // heap node must answer no rather than reading a field at a guessed
    // offset.
    Arena arena;
    Queue<> queue(4);
    PriorityQueue<> heap(4);
    RandomBag<> bag(4);
    CacheEntry* a = arena.add(1);
    CHECK(ok(heap.insert(a, 5)));
    CHECK(!queue.contains(a));
    CHECK(!bag.contains(a));
    CHECK(heap.contains(a));
  }

  TEST_CASE("Queue: moveToFront and moveToBack") {
    Arena arena;
    Queue<> queue(8);
    CacheEntry* a = arena.add(1);
    CacheEntry* b = arena.add(2);
    CacheEntry* c = arena.add(3);
    queue.pushBack(a);
    queue.pushBack(b);
    queue.pushBack(c);  // a, b, c
    CHECK(ok(queue.moveToFront(b)));  // b, a, c
    CHECK_EQ(queue.front(), b);
    CHECK(ok(queue.moveToBack(b)));  // a, c, b
    CHECK_EQ(queue.back(), b);
    CHECK_EQ(queue.front(), a);
    // Moving the end element to its own end is a no-op, not a corruption.
    CHECK(ok(queue.moveToBack(b)));
    CHECK_EQ(queue.back(), b);
    CHECK_EQ(queue.size(), std::size_t{3});
  }

  TEST_CASE("Queue: popBack and popFront detach the entry") {
    Arena arena;
    Queue<> queue(8);
    CacheEntry* a = arena.add(1);
    CacheEntry* b = arena.add(2);
    queue.pushBack(a);
    queue.pushBack(b);
    CHECK_EQ(queue.popBack(), b);
    CHECK(b->metadata == nullptr);
    CHECK_EQ(queue.popFront(), a);
    CHECK(a->metadata == nullptr);
    CHECK(queue.empty());
    CHECK(queue.popBack() == nullptr);
    CHECK(queue.popFront() == nullptr);
  }

  TEST_CASE("Queue: byte total tracks links, unlinks and resizes") {
    Arena arena;
    Queue<> queue(8);
    CacheEntry* a = arena.add(1, 100);
    CacheEntry* b = arena.add(2, 250);
    queue.pushFront(a);
    queue.pushFront(b);
    CHECK_EQ(queue.bytes(), std::uint64_t{350});
    queue.adjustBytes(-50);  // as a policy's onResize would
    CHECK_EQ(queue.bytes(), std::uint64_t{300});
    queue.unlink(b);
    CHECK_EQ(queue.bytes(), std::uint64_t{50});
  }

  TEST_CASE("Queue: payload survives moves within the queue") {
    Arena arena;
    Queue<int> queue(8);
    CacheEntry* a = arena.add(1);
    CacheEntry* b = arena.add(2);
    queue.pushFront(a);
    queue.pushFront(b);
    queue.payload(a) = 42;
    queue.payload(b) = 7;
    CHECK(ok(queue.moveToBack(a)));
    CHECK(ok(queue.moveToFront(a)));
    CHECK_EQ(queue.payload(a), 42);
    CHECK_EQ(queue.payload(b), 7);
    // payloadOf reaches the same field without naming the owning instance,
    // which is what LFU relies on to find an object's bucket.
    CHECK_EQ(Queue<int>::payloadOf(a), 42);
  }

  TEST_CASE("Queue: a fresh node starts with a default payload") {
    Arena arena;
    Queue<int> queue(2);
    CacheEntry* a = arena.add(1);
    queue.pushFront(a);
    queue.payload(a) = 99;
    queue.unlink(a);
    // The node is recycled from the free list; its payload must not be.
    queue.pushFront(a);
    CHECK_EQ(queue.payload(a), 0);
  }

  TEST_CASE("Queue: tag carries per-queue state") {
    Queue<NoData, std::int64_t> bucket(4, 7);
    CHECK_EQ(bucket.tag(), std::int64_t{7});
    bucket.setTag(9);
    CHECK_EQ(bucket.tag(), std::int64_t{9});
  }

  TEST_CASE("Queue: clear detaches every entry") {
    Arena arena;
    Queue<> queue(8);
    std::vector<CacheEntry*> entries;
    for (std::uint64_t i = 0; i < 20; ++i) {
      entries.push_back(arena.add(i));
      queue.pushFront(entries.back());
    }
    queue.clear();
    CHECK(queue.empty());
    CHECK_EQ(queue.size(), std::size_t{0});
    CHECK_EQ(queue.bytes(), std::uint64_t{0});
    for (CacheEntry* entry : entries) CHECK(entry->metadata == nullptr);
    // And the queue is usable afterwards.
    CHECK(ok(queue.pushFront(entries[0])));
  }

  TEST_CASE("Queue: FIFO order holds over a long random workload") {
    Arena arena(4096);
    Queue<> queue(64);
    std::vector<CacheEntry*> expected;  // front to back
    Rng rng(1234);
    for (std::uint64_t i = 0; i < 4000; ++i) {
      const std::uint64_t action = rng.below(4);
      if (action < 2 || expected.empty()) {
        CacheEntry* entry = arena.add(i);
        if (rng.below(2) == 0) {
          queue.pushFront(entry);
          expected.insert(expected.begin(), entry);
        } else {
          queue.pushBack(entry);
          expected.push_back(entry);
        }
      } else if (action == 2) {
        CacheEntry* popped = queue.popBack();
        CHECK_EQ(popped, expected.back());
        expected.pop_back();
      } else {
        const std::size_t index = static_cast<std::size_t>(rng.below(expected.size()));
        CacheEntry* target = expected[index];
        queue.moveToFront(target);
        expected.erase(expected.begin() + static_cast<std::ptrdiff_t>(index));
        expected.insert(expected.begin(), target);
      }
      CHECK_EQ(queue.size(), expected.size());
    }
    // Walk the queue and compare the whole order, not just the ends.
    std::vector<CacheEntry*> actual;
    for (CacheEntry* entry = queue.front(); entry != nullptr; entry = queue.next(entry)) {
      actual.push_back(entry);
    }
    CHECK(actual == expected);
  }
}

void testPriorityQueue() {
  TEST_CASE("PriorityQueue: pops in descending priority order") {
    Arena arena;
    PriorityQueue<> heap(16);
    Rng rng(7);
    std::vector<std::int64_t> priorities;
    for (std::uint64_t i = 0; i < 500; ++i) {
      const auto priority = static_cast<std::int64_t>(rng.below(10000));
      CHECK(ok(heap.insert(arena.add(i), priority)));
      priorities.push_back(priority);
    }
    std::sort(priorities.begin(), priorities.end(), std::greater<>());
    for (const std::int64_t expected : priorities) {
      CacheEntry* top = heap.peek();
      CHECK(top != nullptr);
      CHECK_EQ(heap.priorityOf(top), expected);
      CHECK_EQ(heap.pop(), top);
    }
    CHECK(heap.empty());
    CHECK(heap.pop() == nullptr);
  }

  TEST_CASE("PriorityQueue: updatePriority re-sifts in both directions") {
    Arena arena;
    PriorityQueue<> heap(16);
    CacheEntry* a = arena.add(1);
    CacheEntry* b = arena.add(2);
    CacheEntry* c = arena.add(3);
    heap.insert(a, 10);
    heap.insert(b, 20);
    heap.insert(c, 30);
    CHECK_EQ(heap.peek(), c);
    CHECK(ok(heap.updatePriority(a, 100)));  // up
    CHECK_EQ(heap.peek(), a);
    CHECK(ok(heap.updatePriority(a, 1)));  // down
    CHECK_EQ(heap.peek(), c);
    CHECK(ok(heap.updatePriority(c, 5)));
    CHECK_EQ(heap.peek(), b);
  }

  TEST_CASE("PriorityQueue: arbitrary removal keeps the heap valid") {
    Arena arena(4096);
    PriorityQueue<> heap(64);
    Rng rng(99);
    std::vector<CacheEntry*> live;
    std::vector<std::int64_t> live_priorities;
    for (std::uint64_t i = 0; i < 3000; ++i) {
      if (live.empty() || rng.below(3) != 0) {
        const auto priority = static_cast<std::int64_t>(rng.below(1u << 20));
        CacheEntry* entry = arena.add(i);
        CHECK(ok(heap.insert(entry, priority)));
        live.push_back(entry);
        live_priorities.push_back(priority);
      } else {
        const auto index = static_cast<std::size_t>(rng.below(live.size()));
        CHECK(ok(heap.remove(live[index])));
        CHECK(live[index]->metadata == nullptr);
        live.erase(live.begin() + static_cast<std::ptrdiff_t>(index));
        live_priorities.erase(live_priorities.begin() + static_cast<std::ptrdiff_t>(index));
      }
      // The root must be the maximum at every step — the invariant that
      // arbitrary removal is most likely to break.
      if (!live_priorities.empty()) {
        const std::int64_t expected =
            *std::max_element(live_priorities.begin(), live_priorities.end());
        CHECK_EQ(heap.priorityOf(heap.peek()), expected);
      }
      CHECK_EQ(heap.size(), live.size());
    }
  }

  TEST_CASE("PriorityQueue: a greater<> comparator gives a min-heap") {
    Arena arena;
    PriorityQueue<std::int64_t, NoData, std::greater<std::int64_t>> heap(8);
    CacheEntry* a = arena.add(1);
    CacheEntry* b = arena.add(2);
    heap.insert(a, 10);
    heap.insert(b, 3);
    CHECK_EQ(heap.peek(), b);
    heap.updatePriority(b, 100);
    CHECK_EQ(heap.peek(), a);
  }

  TEST_CASE("PriorityQueue: payload and byte total") {
    Arena arena;
    PriorityQueue<std::int64_t, int> heap(8);
    CacheEntry* a = arena.add(1, 64);
    heap.insert(a, 1);
    heap.payload(a) = 5;
    CHECK_EQ(heap.payload(a), 5);
    CHECK_EQ(heap.bytes(), std::uint64_t{64});
    heap.remove(a);
    CHECK_EQ(heap.bytes(), std::uint64_t{0});
  }

  TEST_CASE("PriorityQueue: inserting an already-tracked entry fails") {
    Arena arena;
    PriorityQueue<> heap(8);
    CacheEntry* a = arena.add(1);
    CHECK(ok(heap.insert(a, 1)));
    CHECK(!ok(heap.insert(a, 2)));
  }
}

void testIdHistory() {
  TEST_CASE("IdHistory: records, finds, and ages out the oldest") {
    IdHistory history(3);
    history.record(1, 10);
    history.record(2, 20);
    history.record(3, 30);
    CHECK_EQ(history.size(), std::size_t{3});
    CHECK_EQ(history.bytes(), std::uint64_t{60});
    CHECK(history.contains(1));
    history.record(4, 40);
    // 1 was the oldest, so it is the one that went.
    CHECK(!history.contains(1));
    CHECK(history.contains(2));
    CHECK(history.contains(4));
    CHECK_EQ(history.size(), std::size_t{3});
    CHECK_EQ(history.bytes(), std::uint64_t{90});
    CHECK_EQ(history.sizeOf(4), std::uint32_t{40});
    CHECK_EQ(history.sizeOf(1), std::uint32_t{0});
  }

  TEST_CASE("IdHistory: recording a known id is a no-op") {
    IdHistory history(3);
    history.record(1, 10);
    history.record(1, 999);
    CHECK_EQ(history.size(), std::size_t{1});
    CHECK_EQ(history.sizeOf(1), std::uint32_t{10});
  }

  TEST_CASE("IdHistory: popOldest returns FIFO order") {
    IdHistory history(4);
    for (std::uint64_t i = 1; i <= 4; ++i) history.record(i, 1);
    for (std::uint64_t i = 1; i <= 4; ++i) {
      const auto popped = history.popOldest();
      CHECK(popped.has_value());
      CHECK_EQ(popped->obj_id, i);
    }
    CHECK(!history.popOldest().has_value());
  }

  TEST_CASE("IdHistory: capacity bounds live ids, not slots") {
    // This is the bug the ring-buffer version had: with removal by id as the
    // dominant operation, a slot-bounded structure drops live ids long before
    // reaching its nominal capacity, and ARC silently adapts on a directory
    // that has lost entries.
    IdHistory history(8);
    for (std::uint64_t round = 0; round < 1000; ++round) {
      history.record(round, 1);
      if (round % 2 == 0) CHECK(history.erase(round));
      CHECK(history.size() <= 8);
    }
    // Fill it up with ids that are never erased, interleaved with churn.
    IdHistory steady(8);
    for (std::uint64_t i = 0; i < 8; ++i) steady.record(1000 + i, 1);
    for (std::uint64_t round = 0; round < 100; ++round) {
      steady.record(round, 1);
      CHECK(steady.erase(round));
      // The eight permanent ids must not be displaced by churn that is
      // removed again straight away... except for the one the record()
      // displaced to make room, which is unavoidable and correct.
      CHECK(steady.size() == 7 || steady.size() == 8);
    }
  }

  TEST_CASE("IdHistory: erase, newest, oldest, clear") {
    IdHistory history(4);
    history.record(1, 1);
    history.record(2, 2);
    CHECK_EQ(history.newest()->obj_id, std::uint64_t{2});
    CHECK_EQ(history.oldest()->obj_id, std::uint64_t{1});
    CHECK(history.erase(1));
    CHECK(!history.erase(1));
    CHECK_EQ(history.oldest()->obj_id, std::uint64_t{2});
    CHECK_EQ(history.bytes(), std::uint64_t{2});
    history.clear();
    CHECK(history.empty());
    CHECK_EQ(history.bytes(), std::uint64_t{0});
    CHECK(!history.newest().has_value());
    history.record(5, 5);
    CHECK(history.contains(5));
  }

  TEST_CASE("IdHistory: zero capacity records nothing") {
    IdHistory history(0);
    history.record(1, 1);
    CHECK(history.empty());
    CHECK(!history.contains(1));
  }
}

void testRandomBag() {
  TEST_CASE("RandomBag: insert, remove, contains") {
    Arena arena;
    RandomBag<> bag(16);
    CacheEntry* a = arena.add(1, 5);
    CacheEntry* b = arena.add(2, 7);
    CHECK(ok(bag.insert(a)));
    CHECK(ok(bag.insert(b)));
    CHECK(!ok(bag.insert(a)));
    CHECK_EQ(bag.size(), std::size_t{2});
    CHECK_EQ(bag.bytes(), std::uint64_t{12});
    CHECK(bag.contains(a));
    CHECK(ok(bag.remove(a)));
    CHECK(!bag.contains(a));
    CHECK(a->metadata == nullptr);
    CHECK_EQ(bag.bytes(), std::uint64_t{7});
  }

  TEST_CASE("RandomBag: popRandom drains without repeating") {
    Arena arena(2048);
    RandomBag<> bag(64);
    std::set<std::uint64_t> inserted;
    for (std::uint64_t i = 0; i < 500; ++i) {
      bag.insert(arena.add(i));
      inserted.insert(i);
    }
    std::set<std::uint64_t> popped;
    while (!bag.empty()) {
      CacheEntry* entry = bag.popRandom();
      CHECK(entry != nullptr);
      CHECK(popped.insert(entry->objId()).second);
    }
    CHECK(popped == inserted);
  }

  TEST_CASE("RandomBag: removal in the middle keeps every member reachable") {
    Arena arena(2048);
    RandomBag<> bag(64);
    Rng rng(5);
    std::vector<CacheEntry*> live;
    for (std::uint64_t i = 0; i < 2000; ++i) {
      if (live.empty() || rng.below(3) != 0) {
        CacheEntry* entry = arena.add(i);
        bag.insert(entry);
        live.push_back(entry);
      } else {
        const auto index = static_cast<std::size_t>(rng.below(live.size()));
        CHECK(ok(bag.remove(live[index])));
        live.erase(live.begin() + static_cast<std::ptrdiff_t>(index));
      }
      CHECK_EQ(bag.size(), live.size());
    }
    for (CacheEntry* entry : live) CHECK(bag.contains(entry));
  }

  TEST_CASE("RandomBag: sampling is roughly uniform") {
    Arena arena;
    RandomBag<> bag(16);
    constexpr std::size_t kMembers = 10;
    for (std::uint64_t i = 0; i < kMembers; ++i) bag.insert(arena.add(i));
    std::vector<int> counts(kMembers, 0);
    constexpr int kDraws = 100000;
    for (int i = 0; i < kDraws; ++i) {
      ++counts[static_cast<std::size_t>(bag.sample()->objId())];
    }
    // Each member should come up about a tenth of the time. The bound is
    // loose enough never to flake (the seed is fixed anyway) and tight enough
    // to catch an off-by-one that makes one member unreachable.
    for (const int count : counts) {
      CHECK_NEAR(static_cast<double>(count) / kDraws, 1.0 / kMembers, 0.01);
    }
  }

  TEST_CASE("RandomBag: sampleSome draws without replacement") {
    Arena arena;
    RandomBag<> bag(32);
    for (std::uint64_t i = 0; i < 20; ++i) bag.insert(arena.add(i));
    std::set<std::uint64_t> seen;
    bag.sampleSome(8, [&](CacheEntry* entry, NoData&) { seen.insert(entry->objId()); });
    CHECK_EQ(seen.size(), std::size_t{8});
    // Asking for more than there is yields everything, once each.
    seen.clear();
    bag.sampleSome(100, [&](CacheEntry* entry, NoData&) { seen.insert(entry->objId()); });
    CHECK_EQ(seen.size(), std::size_t{20});
    CHECK_EQ(bag.size(), std::size_t{20});
  }
}

void testFrequencySketch() {
  TEST_CASE("FrequencySketch: counts saturate at 15") {
    FrequencySketch sketch(1024);
    for (int i = 0; i < 100; ++i) sketch.increment(42);
    CHECK_EQ(static_cast<int>(sketch.estimate(42)), 15);
  }

  TEST_CASE("FrequencySketch: never under-estimates") {
    // A count-min sketch may over-estimate (through collisions) but must
    // never under-estimate. That is the property TinyLFU's admission decision
    // depends on.
    FrequencySketch sketch(4096);
    Rng rng(3);
    std::vector<int> truth(200, 0);
    for (int i = 0; i < 2000; ++i) {
      const auto id = static_cast<std::uint64_t>(rng.below(200));
      sketch.increment(id);
      if (truth[id] < 15) ++truth[id];
    }
    for (std::uint64_t id = 0; id < 200; ++id) {
      CHECK(sketch.estimate(id) >= static_cast<std::uint8_t>(std::min(truth[id], 15)));
    }
  }

  TEST_CASE("FrequencySketch: separates hot from cold") {
    FrequencySketch sketch(4096);
    for (int i = 0; i < 50; ++i) sketch.increment(7);
    sketch.increment(9);
    CHECK(sketch.estimate(7) > sketch.estimate(9));
  }

  TEST_CASE("FrequencySketch: halving keeps the estimate recent") {
    // Enough traffic to trigger several resets; the hot id must stay hot and
    // an id touched once long ago must decay away.
    FrequencySketch sketch(64);
    sketch.increment(1);
    for (int round = 0; round < 20000; ++round) {
      sketch.increment(static_cast<std::uint64_t>(1000 + round));
      sketch.increment(2);
    }
    CHECK(sketch.estimate(2) > sketch.estimate(1));
  }

  TEST_CASE("FrequencySketch: clear resets every counter") {
    FrequencySketch sketch(256);
    for (int i = 0; i < 20; ++i) sketch.increment(5);
    sketch.clear();
    CHECK_EQ(static_cast<int>(sketch.estimate(5)), 0);
  }
}

}  // namespace

int main() {
  std::printf("CacheStructure\n");
  testCacheStructure();
  std::printf("Queue\n");
  testQueue();
  std::printf("PriorityQueue\n");
  testPriorityQueue();
  std::printf("IdHistory\n");
  testIdHistory();
  std::printf("RandomBag\n");
  testRandomBag();
  std::printf("FrequencySketch\n");
  testFrequencySketch();
  return cachesim::test::summarize("datastructures_test");
}
