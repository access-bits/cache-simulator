# `include/internal/datastructures/`

The intrusive structures every eviction policy is built from, and the one idea
that makes them interchangeable.

| Header | Structure | Use it for |
|---|---|---|
| `metadata.hpp` | `MetadataNode` | The shared base of every node type. Not used directly. |
| `block_pool.hpp` | `BlockPool<T>` | Pointer-stable node allocation. Used *by* the structures below. |
| `queue.hpp` | `Queue<Payload, Tag>` | Any ordering: LRU, FIFO, a frequency bucket, a segment, a ghost-adjacent list. |
| `priority_queue.hpp` | `PriorityQueue<Priority, Payload, Compare>` | "Give me the extreme object by a key that changes." Belady, LFU-DA, LRU-K. |
| `random_bag.hpp` | `RandomBag<Payload>` | Uniform random victim in O(1); sampled eviction. |
| `id_history.hpp` | `IdHistory` | Ghost lists — remembering objects that are no longer cached. |
| `frequency_sketch.hpp` | `FrequencySketch` | Bounded-memory frequency estimates, including for objects not cached. TinyLFU. |

---

## The metadata pointer

Everything here rests on one arrangement. A `CacheEntry` has a single pointer
to a node owned by whichever structure currently orders it, and that node
points back:

```
   obj_id ──(hash index)──▶ CacheEntry ──metadata──▶ QueueEntry
                                 ▲                   { owner, prev, next, payload }
                                 └──cache_entry──────┘
```

Both directions are O(1) and neither involves a hash lookup, which is what
makes an LRU hit two pointer writes instead of a map probe. An entry is only
ever in one such structure at a time, so there is no contention for the single
pointer — and a policy that needs several (ARC's T1 and T2, S3FIFO's three
queues, LFU's one-bucket-per-frequency) simply moves the entry between them.

Moving an entry between structures allocates a fresh node, so **the payload
does not travel with it**. A policy carrying per-object state across a move
has to read it before unlinking and write it after pushing — S3FIFO does
exactly this with its 2-bit counter.

### Why the pointer is typed

It was `void*`, and every structure answered "is this entry mine?" by casting
it to its own node type and reading an `owner` field at a guessed offset. That
is undefined behaviour the moment the guess is wrong, and it happened to
produce the right answer only because `Queue`'s and `PriorityQueue`'s nodes
coincidentally kept that field at the same offset — a coincidence that breaks
as soon as a policy holds two different kinds of structure at once, which
several of them now do.

So every node type derives from `MetadataNode`, whose first member is the
owner pointer. The question is answerable from the base pointer with no
guessing: read `owner`, compare, and only then downcast.

```cpp
bool contains(const CacheEntry* e) const {
  return e != nullptr && e->metadata != nullptr && e->metadata->owner == this;
}
```

`datastructures_test.cpp` pins this down directly: an entry in a
`PriorityQueue` must make `Queue::contains` and `RandomBag::contains` both
answer *no*.

---

## `BlockPool<T>`

Hands out addresses that stay valid for the pool's lifetime, with a free list
for reuse. Every structure here needs exactly this, because nodes are reached
through `CacheEntry::metadata` and a simulation does millions of
allocate/release pairs per second.

**Why not `std::deque`,** which is also pointer-stable: libstdc++ sizes its
chunks at 512 bytes, which for a 32-byte node is 16 nodes per heap block — an
allocation every 16 nodes while warming up, and a node stream that is only
ever contiguous 16 at a time. Here the first block is sized from the caller's
hint, so the usual case is one allocation and one contiguous run.

`T` must be default-constructible and should be trivially destructible —
`release()` is a free-list push, not a destructor call.

## `Queue<Payload, Tag>`

An intrusive doubly-linked list with two optional pieces of caller-defined
state:

- **`Payload`** — per *object*, carried in its node. `Queue<bool>` is Sieve's
  visited bit; `Queue<std::uint8_t>` is S3FIFO's 2-bit counter and SLRU's
  segment index; `Queue<std::int64_t>` is LFU's per-object frequency.
- **`Tag`** — per *queue*, describing the list as a whole. LFU's buckets are
  `Queue<std::int64_t, std::int64_t>`, tagged with the frequency they hold.

Both cost nothing when unused (`[[no_unique_address]]` on `NoData`). A node
with no payload is 32 bytes: owner, cache_entry, prev, next.

```cpp
Status pushFront(CacheEntry*);   Status pushBack(CacheEntry*);
Status moveToFront(CacheEntry*); Status moveToBack(CacheEntry*);
Status unlink(CacheEntry*);
CacheEntry* popFront();          CacheEntry* popBack();
CacheEntry* front() const;       CacheEntry* back() const;
CacheEntry* next(const CacheEntry*) const;   // towards the back
CacheEntry* prev(const CacheEntry*) const;   // towards the head
bool contains(const CacheEntry*) const;
Payload& payload(CacheEntry*);
static Payload& payloadOf(CacheEntry*);      // see below
std::size_t size() const;  std::uint64_t bytes() const;
void adjustBytes(std::int64_t delta);
void clear();
```

`popBack()` exists rather than leaving callers to write `back()` + `unlink()`,
which is every LRU-shaped policy's eviction path and would otherwise repeat
the ownership check.

`next`/`prev` let a policy walk the ordering — Sieve's hand, Clock's sweep —
without the node type being visible.

**`payloadOf` is a static with a weaker precondition** than `payload`: the
entry's node must be *this `Queue` specialization's* node type, but not
necessarily owned by this instance. It exists for LFU, which keeps one bucket
per frequency and has to read an object's frequency *in order to find out
which bucket it is in* — so it cannot ask the owning bucket first. The payload
sits at a fixed offset in the node, so this is a field load, not a search over
buckets, and it is the difference between an O(1) and an O(number of
frequencies) hit path.

**`bytes()` and `adjustBytes`.** Policies that split a byte capacity between
queues (ARC's target, S3FIFO's 10/90, SLRU's segments) need a running byte
total, and keeping it here is O(1) per link rather than O(n) per query. When a
tracked object's size changes, only the policy is told (its `onResize` hook)
and only the queue knows the running total, so `adjustBytes` is where the two
meet.

**Not movable or copyable.** Nodes record their owning queue's address, so
moving a `Queue` would leave every node pointing at the old address. This is
enforced at compile time, which is what catches the natural mistake of writing
`map<int64_t, Queue>` for LFU's buckets — a map that stores values inline
moves them when it grows. The buckets are `unique_ptr<Queue>` for exactly this
reason.

## `PriorityQueue<Priority, Payload, Compare>`

An addressable binary heap. *Addressable* is the whole point:
`CacheEntry::metadata` points straight at the object's heap node, so when the
object is accessed again its key can be changed in place with an O(log n)
re-sift instead of an O(n) search for where in the heap it currently sits.
Belady is the motivating case — every hit revises that object's next-access
time — and it is unusable without this property.

```cpp
Status insert(CacheEntry*, Priority);
Status updatePriority(CacheEntry*, Priority);   // O(log n)
CacheEntry* peek() const;  CacheEntry* pop();
Status remove(CacheEntry*);                     // arbitrary, not just the root
const Priority& priorityOf(const CacheEntry*) const;
```

The root is the element that compares greatest under `Compare`, so
`std::less` (the default) is a max-heap, matching `std::priority_queue`, and
`std::greater` gives a min-heap. Belady wants a max-heap on
`next_access_vtime` — the object used furthest in the future is the best
victim, and `kNeverAgain == INT64_MAX` sorts above every real time with no
special case.

The sifts move a *hole* rather than swapping pairs: one write per level
instead of three, which matters because every Belady hit does a full sift.

Composite keys work and are used: LRU-K's priority is
`pair<kth_reference_time, last_reference_time>` under `std::greater`, giving a
min-heap whose tie-break is recency.

## `RandomBag<Payload>`

A dense vector of members plus each member's index in that vector, so removing
an arbitrary entry is a swap with the last element and two index writes.

```cpp
Status insert(CacheEntry*);  Status remove(CacheEntry*);
CacheEntry* popRandom();     CacheEntry* sample();
template <typename Fn> void sampleSome(std::size_t count, Fn&& fn);
```

`sampleSome` draws `count` distinct members by partial Fisher–Yates over the
dense array — without replacement, and leaving the array a valid permutation
of itself. That is the primitive for sampled eviction: a policy whose victim
score depends on the current time cannot keep a heap (the ranking changes
every request), but it can look at a few dozen random objects and take the
worst, which for any reasonable score gets within a hair of the true worst.

## `IdHistory`

A bounded FIFO of object ids with O(1) membership, insertion, removal-by-id
and removal-of-the-oldest. This is the ghost-list structure: ARC, 2Q and
S3FIFO all remember something about objects that have been evicted, and an
evicted object has no `CacheEntry` left to hang metadata off — its slot has
been recycled. So unlike everything above, this one works with bare ids.

```cpp
void record(std::uint64_t obj_id, std::uint32_t size = 0);
bool contains(std::uint64_t) const;
bool erase(std::uint64_t);
std::optional<Record> popOldest();
std::uint64_t bytes() const;    std::size_t size() const;
```

Entries carry the size the object had when recorded, plus a running byte
total, because ARC's adaptation compares its ghost lists against the cache's
capacity and on a byte-capacity cache "size" has to mean bytes — which cannot
be recovered from an id after the object is gone.

### The design that did not work, and why it is worth knowing

The obvious cheaper implementation is a ring buffer of ids plus an
`id → slot` map, with a tombstone left behind on removal so that every
operation stays O(1). **It is wrong**, and the way it is wrong is instructive:
the capacity then bounds *slots* rather than *live ids*.

Removal by id is the dominant operation for a ghost list — almost every ghost
leaves by being promoted back into the cache, not by ageing out. So the ring
fills with tombstones scattered through it, reaches its bound while only a
fraction of its ids are live, and starts evicting live ghosts to make room.
ARC then adapts `p` on a directory that has quietly lost entries, which costs
a point or two of hit ratio with nothing whatsoever to indicate that anything
is wrong. It was caught only by cross-validating against the FAST'03
pseudocode, and only at capacity 64 and above.

The current implementation is an intrusive doubly-linked list over pooled
nodes plus a map: every operation is still O(1), the capacity bounds exactly
what it promises, and there is nothing to compact.

## `FrequencySketch`

A count-min sketch with 4-bit saturating counters and periodic halving — the
frequency estimator TinyLFU is built on.

The problem it solves: LFU-style admission needs to know how often an object
is used, but keeping an exact counter per object means keeping state for
objects that are *not cached* (otherwise a one-hit object admitted now looks
exactly like a popular object admitted now), and that state grows with the
whole trace rather than with the cache. The sketch keeps a fixed amount of
memory and answers with a bounded over-estimate.

Two properties make it work, and both are load-bearing:

- **Four rows, minimum taken.** Each row hashes the id to one counter and
  counts collisions too, so each row over-estimates; the smallest of four
  independent over-estimates makes a large error unlikely.
- **Halving.** Every counter is halved once the sketch has absorbed ten
  increments per cache slot, so the estimate tracks *recent* popularity. Without
  it the sketch converges to the trace's lifetime distribution and stops
  adapting — which is exactly LFU's classic failure mode.

**Sizing is not a free parameter.** Each row gets four counters per cache
entry, so sixteen per entry in total, which at half a byte each is eight bytes
per cache slot. With only one counter per entry per row, a trace whose
distinct-object count is far above the cache size — which is every interesting
trace — collides so heavily that most estimates saturate, every admission
comparison becomes a tie, and the filter stops filtering. The observable
symptom is a W-TinyLFU that scores *exactly* LRU's hit ratio and does not
respond to any of its own parameters. That is one of the five bugs the test
suite found.

---

## Choosing between them

| If the policy needs... | Use |
|---|---|
| an ordering, with O(1) ends | `Queue` |
| the extreme element by a key that changes on access | `PriorityQueue` |
| a uniformly random victim, or sampled candidates | `RandomBag` |
| memory of objects it no longer holds | `IdHistory` |
| frequency estimates including for objects it does not hold | `FrequencySketch` |
| per-object state while the object is cached | the owning structure's `Payload` |
| state describing a whole list | that `Queue`'s `Tag` |
| several disjoint sets of cached objects | several structures, moving entries between them |
