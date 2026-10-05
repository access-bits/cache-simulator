# `include/policies/`

One header per eviction policy. This is where new work normally goes: a policy
touches no other file, and registering it is one call.

| Header | Policies |
|---|---|
| `lru.hpp` | `Lru`, `Fifo`, `Mru` |
| `random.hpp` | `Random` |
| `clock.hpp` | `Clock` |
| `sieve.hpp` | `Sieve` |
| `lfu.hpp` | `Lfu` |
| `lfuda.hpp` | `LfuDa` |
| `slru.hpp` | `Slru` |
| `two_q.hpp` | `TwoQ` |
| `arc.hpp` | `Arc` |
| `s3fifo.hpp` | `S3Fifo` |
| `lru_k.hpp` | `LruK` |
| `wtinylfu.hpp` | `WTinyLfu` |
| `belady.hpp` | `Belady` |

Each header's comment explains the algorithm, cites its paper, and says what
problem it solves that the simpler ones do not. The parameter table is in the
[top-level README](../../README.md#eviction-policies).

---

## The interface

```cpp
class IEvictionPolicy {
 public:
  virtual std::string name() const = 0;

  virtual Status onAttach(internal::CacheStructure&);          // optional
  virtual Status onHit(internal::CacheEntry*, const Request&) = 0;
  virtual Status onMiss(const Request&);                       // optional
  virtual Status onAdmit(internal::CacheEntry*, const Request&) = 0;
  virtual internal::CacheEntry* evict() = 0;
  virtual Status onRemove(internal::CacheEntry*) = 0;
  virtual Status onResize(internal::CacheEntry*, std::uint32_t old, std::uint32_t New);
  virtual void clear();
  virtual std::uint64_t metadataBytes() const;                 // diagnostic
};
```

### Call order for one request

```
hit:    onHit(entry, req)
        └─ if the request named a new size: onResize(entry, old, new),
           then possibly evict()

miss:   onMiss(req)  ──▶  evict() ... (zero or more)  ──▶  onAdmit(entry, req)
```

### What each hook guarantees

| Hook | Contract |
|---|---|
| `onHit` | The entry is cached and tracked by you. Its size has already been updated if the request named a new one. |
| `onMiss` | Runs **before** any eviction and before `onAdmit`. This is where an adaptive policy consults its ghost list and adapts, because that decision changes which list the following `evict()` should take from. Ordering it after eviction silently breaks ARC, 2Q and S3FIFO. |
| `onAdmit` | The entry exists in the cache structure and its `metadata` is `nullptr`. You must start tracking it, which is what sets `metadata`. |
| `evict` | Choose a victim, **detach it** (so its `metadata` is back to `nullptr`), and return it. Return `nullptr` only if you genuinely track nothing; the engine only asks when it needs space, and treats `nullptr` as fatal. |
| `onRemove` | An entry is leaving for a reason other than eviction — an explicit invalidation, or an object that grew past the whole capacity. Detach it, exactly as `evict` would. |
| `onResize` | A tracked object's size changed. The cache structure's accounting is already fixed; this is your chance to fix byte totals of your own, which your queues cannot do themselves because only you are told it happened. Ignore it if you only count objects. |
| `clear` | Drop all state. Every `CacheEntry*` you hold is about to be recycled, so a policy whose state is in its queues must empty them here. |
| `onAttach` | Called once before the first request, with the `CacheStructure`. Only needed if you want the structure itself — e.g. to scan every cached object. |

### Why `evict()` both selects and detaches

The earlier interface was `selectVictim()` (const, no side effects) plus
`onEvict(entry)`. Clock, Sieve and S3FIFO cannot honour that: all three mutate
state while *scanning* for a victim — advancing a hand, clearing reference
bits, promoting an object from the small queue to the main one. The split
forced them either to lie about constness or to scan twice. Merging them also
halves the virtual calls on the eviction path.

### Why configuration arrives at construction

```cpp
struct PolicyConfig {
  std::uint64_t capacity_bytes;
  std::size_t   entry_hint;     // expected peak object count
  ParamMap      params;         // "small-size-ratio=0.1,..."
  std::uint64_t seed;
};
```

Most policies need capacity *before* they can build anything: ARC sizes its
ghost lists from it, S3FIFO splits it 10/90, SLRU divides it into segments,
every `Queue` reserves its node pool from `entry_hint`. Handing it over in the
constructor leaves `onAttach` for the one thing it is actually for.

`entry_hint` is a sizing hint, never a cap — a policy must stay correct if the
real object count exceeds it.

`seed` exists so that a run involving any random choice is reproducible.

---

## Writing one

```cpp
#pragma once
#include <string>
#include "internal/datastructures/queue.hpp"
#include "internal/eviction/eviction_policy.hpp"

namespace cachesim::policies {

// Evicts the least recently used object, but gives an object a second
// chance if it was hit at least `threshold` times. (An illustration, not a
// published algorithm.)
class SecondLook final : public IEvictionPolicy {
 public:
  explicit SecondLook(const PolicyConfig& config = {})
      : queue_(config.entry_hint),
        threshold_(static_cast<std::uint8_t>(config.params.getInt("threshold", 2))) {}

  std::string name() const override {
    return "SecondLook-" + std::to_string(threshold_);   // see note below
  }

  Status onAdmit(internal::CacheEntry* entry, const Request&) override {
    const Status status = queue_.pushFront(entry);
    if (!ok(status)) return status;
    queue_.payload(entry) = 0;
    return Status::kSuccess;
  }

  Status onHit(internal::CacheEntry* entry, const Request&) override {
    if (!queue_.contains(entry)) return Status::kFailure;
    std::uint8_t& hits = queue_.payload(entry);
    if (hits < 255) ++hits;
    return queue_.moveToFront(entry);
  }

  internal::CacheEntry* evict() override {
    for (;;) {
      internal::CacheEntry* candidate = queue_.back();
      if (candidate == nullptr) return nullptr;
      std::uint8_t& hits = queue_.payload(candidate);
      if (hits < threshold_) return queue_.popBack();
      hits = 0;                                   // spend the credit
      if (!ok(queue_.moveToFront(candidate))) return nullptr;
    }
  }

  Status onRemove(internal::CacheEntry* entry) override { return queue_.unlink(entry); }

  Status onResize(internal::CacheEntry*, std::uint32_t old_size,
                  std::uint32_t new_size) override {
    queue_.adjustBytes(static_cast<std::int64_t>(new_size) -
                       static_cast<std::int64_t>(old_size));
    return Status::kSuccess;
  }

  void clear() override { queue_.clear(); }

 private:
  Queue<std::uint8_t> queue_;   // payload = hits since last considered
  std::uint8_t threshold_;
};

}  // namespace cachesim::policies
```

Register it:

```cpp
PolicyRegistry::instance().add({
    "SecondLook",                    // canonical name, as it appears in results
    {"sl", "second-look"},           // spellings accepted from a config
    "LRU with a hit threshold; threshold=N",
    [](const PolicyConfig& config) {
      return std::make_unique<policies::SecondLook>(config);
    }});
```

A later registration under an existing name **replaces** it, so you can
override a built-in with your own version.

To ship it with the simulator instead, add the header and one `registry.add`
line to `src/internal/eviction/policy_registry.cpp`.

---

## Things that will bite you

**Return the name with its distinguishing parameters.** `LRU-K` reports
`LRU-2` or `LRU-4`, `Clock` reports `Clock` or `Clock-3`. A sweep over a
policy's own parameters otherwise produces rows that cannot be told apart —
and the point of a sweep is to compare them.

**Terminate your eviction loop.** A policy whose `evict()` can do work without
producing a victim (Clock's second chance, S3FIFO's promotion, W-TinyLFU's
window drain) must loop until an object actually leaves. Each iteration has to
make progress: spend a counter, move one object, or return. Clock and S3FIFO
spend a credit per iteration, so the total work over a run is bounded by the
hits that granted those credits. Sieve bounds its sweep to one lap explicitly,
because with every object marked there is no zero bit to find and it would
otherwise spin.

**Payload does not survive a move between structures.** Moving an entry
allocates a fresh node. Read the payload before unlinking and write it after
pushing — S3FIFO does this with its 2-bit counter.

**Use `contains()` before `payload()`.** `payload()` has a precondition and
does not check it. The static `payloadOf()` is weaker still: it only requires
that the node is this `Queue` specialization's node type. It exists for LFU,
which has to read an object's frequency *in order to find* which bucket holds
it, and using it elsewhere is almost always a mistake.

**One structure can hold an entry at a time.** `pushFront` on an
already-tracked entry fails rather than orphaning the node it already has. If
you keep several queues, unlink before you push.

**Keep byte totals right under resize.** If you split capacity between
structures, implement `onResize` and forward the delta with `adjustBytes`.
Otherwise your splits drift from reality on any trace where objects change
size.

---

## Testing a new policy

`tests/policy_test.cpp` applies three layers, and the first two are free:

- **Universal invariants** run over every registered policy automatically —
  occupancy checked against capacity on *every* request, a capacity of 1 must
  not deadlock, a cache larger than the working set must evict nothing and hit
  exactly `requests - distinct objects` times. Registering your policy opts it
  in.
- **Known relationships** — Belady bounds every online policy from above, so a
  policy that beats Belady has a bug, and this catches it.
- **Reference cross-validation** is the one worth writing by hand. Implement
  the policy a second time in the test file as naively as you can manage —
  `std::list`, O(n) scans, no cleverness — and assert the hit counts match
  across the trace and capacity grid. Being absurdly slow is the point: the
  reference can be read and checked against the paper by eye.

That last layer found four of the five bugs described in
[`../../tests/README.md`](../../tests/README.md), and not one of them would
have crashed anything.
