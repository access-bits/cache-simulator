#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "internal/cache/cache_structure.hpp"
#include "internal/datastructures/id_history.hpp"
#include "internal/datastructures/priority_queue.hpp"
#include "internal/datastructures/queue.hpp"

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::fprintf(stderr, "FAILED: %s (line %d)\n", #cond, __LINE__); \
      std::exit(1);                                                    \
    }                                                                  \
  } while (0)

using cachesim::IdHistory;
using cachesim::PriorityQueue;
using cachesim::Queue;
using cachesim::internal::CacheEntry;
using cachesim::internal::CacheStructure;

namespace {

void testQueuePayload() {
  CacheStructure structure(/*max_entries=*/4);
  CacheEntry* a = structure.admit(1, 10);
  CacheEntry* b = structure.admit(2, 10);

  Queue<int> queue;  // per-object payload: an int
  CHECK(queue.pushBack(a) == cachesim::Status::kSuccess);
  CHECK(queue.pushBack(b) == cachesim::Status::kSuccess);

  queue.payload(a) = 111;
  queue.payload(b) = 222;
  CHECK(queue.payload(a) == 111);
  CHECK(queue.payload(b) == 222);

  // moveToFront must preserve the payload on the same node.
  CHECK(queue.moveToFront(b) == cachesim::Status::kSuccess);
  CHECK(queue.payload(b) == 222);
  CHECK(queue.front() == b);
}

void testQueueTag() {
  Queue<cachesim::NoData, std::int64_t> bucket(/*reserve_hint=*/0, /*tag=*/int64_t{7});
  CHECK(bucket.tag() == 7);
  bucket.setTag(8);
  CHECK(bucket.tag() == 8);
}

void testPriorityQueue() {
  CacheStructure structure(/*max_entries=*/8);
  CacheEntry* a = structure.admit(1, 1);
  CacheEntry* b = structure.admit(2, 1);
  CacheEntry* c = structure.admit(3, 1);

  PriorityQueue<int64_t> pq;
  CHECK(pq.insert(a, 10) == cachesim::Status::kSuccess);
  CHECK(pq.insert(b, 30) == cachesim::Status::kSuccess);
  CHECK(pq.insert(c, 20) == cachesim::Status::kSuccess);

  CHECK(pq.peek() == b);  // highest priority (30) stays on top

  CHECK(pq.updatePriority(a, 100) == cachesim::Status::kSuccess);
  CHECK(pq.peek() == a);  // a is now the highest

  CHECK(pq.remove(c) == cachesim::Status::kSuccess);
  CHECK(pq.size() == 2);
  CHECK(!pq.contains(c));

  CHECK(pq.pop() == a);
  CHECK(pq.pop() == b);
  CHECK(pq.empty());
}

void testIdHistory() {
  IdHistory history(/*capacity=*/2);
  history.record(1);
  history.record(2);
  CHECK(history.contains(1));
  CHECK(history.contains(2));

  history.record(3);  // capacity 2: evicts the oldest (1)
  CHECK(!history.contains(1));
  CHECK(history.contains(2));
  CHECK(history.contains(3));
}

}  // namespace

int main() {
  testQueuePayload();
  testQueueTag();
  testPriorityQueue();
  testIdHistory();

  std::printf("All tests passed\n");
  return 0;
}
