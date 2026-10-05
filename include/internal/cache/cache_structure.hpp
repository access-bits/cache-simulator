#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "ankerl/unordered_dense.h"
#include "internal/cache/cache_entry.hpp"
#include "internal/common/compiler.hpp"
#include "internal/status.hpp"

namespace cachesim::internal {

// Owns the pool of cache entries and the obj_id -> entry lookup. Purely
// "what is cached, how big is it, and where do I find it" — no notion of
// ordering at all; ordering lives in whichever data structure uses
// CacheEntry::metadata.
//
// Two invariants drive the whole design:
//
// 1. Entry addresses must never move. CacheEntry* values are handed out to
//    policies and stashed inside metadata nodes, so a reallocation that moved
//    them would silently corrupt every policy at once.
//
// 2. No allocation per request. libCacheSim calls malloc once per object
//    insert (and free once per eviction); on a trace with a 90% miss ratio
//    that is an allocator round-trip for nearly every request, and it is one
//    of the two structural reasons that simulator is slow.
//
// A single std::vector reserved once satisfies both, but only by making its
// reserved size a hard cap — exceed it and admit() has to fail, because
// growing would move every entry. That cap is a real hazard: it depends on
// the *smallest* object in the trace, which the caller usually cannot know in
// advance, and getting it wrong aborts a sweep hours in.
//
// So the arena is a list of blocks instead. The first block is sized from the
// caller's hint, which in the overwhelmingly common case (uniform object
// sizes, or an object-count capacity) is the whole arena — identical layout
// and locality to the single-vector version, one contiguous run of entries.
// If the trace turns out to hold more small objects than the hint allowed,
// further blocks are appended: addresses stay valid because existing blocks
// are never touched, and the simulation continues instead of dying. Growth is
// geometric, so a badly wrong hint costs O(log n) allocations in total, not
// one per object.
//
// max_entries is kept as a genuine hard cap, defaulting to "no cap". It is
// there for the case where a caller really does want to bound memory and
// treat exhaustion as an error.
class CacheStructure {
 public:
  // entry_hint: expected peak number of concurrently cached objects, used to
  //   size the first arena block and pre-size the index. Sizing it well is a
  //   performance matter (one allocation instead of several, and no rehashing
  //   mid-run), not a correctness one.
  // max_entries: hard cap on concurrently cached objects; 0 means unlimited.
  explicit CacheStructure(std::size_t entry_hint = 0, std::size_t max_entries = 0)
      : max_entries_(max_entries) {
    // A hint of 0 still needs a sane first block; 1024 entries is 24 KiB.
    std::size_t first_block = entry_hint > 0 ? entry_hint : 1024;
    if (max_entries_ > 0 && first_block > max_entries_) first_block = max_entries_;
    if (first_block == 0) first_block = 1;
    addBlock(first_block);
    index_.reserve(first_block);
    // The free list only holds slots released by erase(); it can never be
    // longer than the arena, and reserving the hint up front keeps eviction
    // free of allocation in steady state.
    free_list_.reserve(first_block);
  }

  CacheStructure(const CacheStructure&) = delete;
  CacheStructure& operator=(const CacheStructure&) = delete;
  CacheStructure(CacheStructure&&) = default;
  CacheStructure& operator=(CacheStructure&&) = default;

  // Returns nullptr if obj_id is not currently cached. The hot path of every
  // single request goes through here.
  [[nodiscard]] CACHESIM_ALWAYS_INLINE CacheEntry* find(std::uint64_t obj_id) const {
    const auto it = index_.find(obj_id);
    return it != index_.end() ? it->second : nullptr;
  }

  [[nodiscard]] bool contains(std::uint64_t obj_id) const { return find(obj_id) != nullptr; }

  // Adds a brand-new object. The caller must have already established that
  // obj_id is not cached (every caller has just done the find() that told it
  // this was a miss, so re-checking here would double the cost of the hot
  // path for no information). Returns nullptr only if a hard max_entries cap
  // was set and has been reached.
  CacheEntry* admit(std::uint64_t obj_id, std::uint32_t size) {
    CacheEntry* entry = allocateSlot();
    if (CACHESIM_UNLIKELY(entry == nullptr)) return nullptr;
    entry->obj_id_ = obj_id;
    entry->size_ = size;
    entry->metadata = nullptr;
    index_.emplace(obj_id, entry);
    occupied_bytes_ += size;
    return entry;
  }

  // Frees the entry back to the pool. The caller must have already unlinked
  // it from whatever was tracking it (metadata back to nullptr) — a policy
  // that forgets leaves a dangling node, so we check and refuse.
  Status erase(CacheEntry* entry) {
    if (CACHESIM_UNLIKELY(entry == nullptr || entry->metadata != nullptr)) {
      return Status::kFailure;
    }
    const auto it = index_.find(entry->obj_id_);
    if (CACHESIM_UNLIKELY(it == index_.end() || it->second != entry)) {
      return Status::kFailure;
    }
    occupied_bytes_ -= entry->size_;
    index_.erase(it);
    free_list_.push_back(entry);
    return Status::kSuccess;
  }

  // Records that a cached object's size changed (traces do re-write an object
  // at a new size). Only the byte accounting moves; identity and the index
  // entry are untouched, so every outstanding CacheEntry* stays valid.
  Status resize(CacheEntry* entry, std::uint32_t new_size) {
    if (CACHESIM_UNLIKELY(entry == nullptr)) return Status::kFailure;
    occupied_bytes_ -= entry->size_;
    occupied_bytes_ += new_size;
    entry->size_ = new_size;
    return Status::kSuccess;
  }

  // Drops every cached object. Slots return to the free list, so the arena's
  // memory is reused rather than released — this exists so a warm-up phase
  // can be discarded without rebuilding the whole structure.
  void clear() {
    for (const auto& [obj_id, entry] : index_) {
      (void)obj_id;
      entry->metadata = nullptr;
      free_list_.push_back(entry);
    }
    index_.clear();
    occupied_bytes_ = 0;
  }

  [[nodiscard]] std::uint64_t occupiedBytes() const { return occupied_bytes_; }
  [[nodiscard]] std::size_t objectCount() const { return index_.size(); }
  [[nodiscard]] std::size_t maxEntries() const { return max_entries_; }

  // Slots ever handed out (live plus free). Diagnostics only: a value far
  // above the hint means the hint was too low and the arena had to grow.
  [[nodiscard]] std::size_t arenaCapacity() const { return arena_capacity_; }
  [[nodiscard]] std::size_t blockCount() const { return blocks_.size(); }

  // Iterates every currently cached entry. Order is the index's internal
  // order, which is neither insertion nor access order — fine for the one
  // thing this is for (a policy or plugin that wants to sweep all objects,
  // e.g. clearing access bits), not something to build ordering on.
  template <typename Fn>
  void forEachEntry(Fn&& fn) const {
    for (const auto& [obj_id, entry] : index_) {
      (void)obj_id;
      fn(entry);
    }
  }

 private:
  struct Block {
    std::unique_ptr<CacheEntry[]> data;
    std::size_t capacity = 0;
    std::size_t used = 0;
  };

  void addBlock(std::size_t capacity) {
    Block block;
    block.data = std::make_unique<CacheEntry[]>(capacity);
    block.capacity = capacity;
    blocks_.push_back(std::move(block));
    arena_capacity_ += capacity;
  }

  CacheEntry* allocateSlot() {
    if (!free_list_.empty()) {
      CacheEntry* entry = free_list_.back();
      free_list_.pop_back();
      return entry;
    }
    Block* block = &blocks_.back();
    if (CACHESIM_UNLIKELY(block->used == block->capacity)) {
      if (max_entries_ > 0 && arena_capacity_ >= max_entries_) return nullptr;
      // Geometric growth, clamped so one bad hint cannot balloon memory:
      // half the arena so far, at least 1024 entries, at most 1M (24 MiB).
      std::size_t next = arena_capacity_ / 2;
      if (next < 1024) next = 1024;
      if (next > (1u << 20)) next = 1u << 20;
      if (max_entries_ > 0) {
        const std::size_t room = max_entries_ - arena_capacity_;
        if (next > room) next = room;
      }
      addBlock(next);
      block = &blocks_.back();
    }
    return &block->data[block->used++];
  }

  std::vector<Block> blocks_;
  std::vector<CacheEntry*> free_list_;
  ankerl::unordered_dense::map<std::uint64_t, CacheEntry*> index_;
  std::uint64_t occupied_bytes_ = 0;
  std::size_t arena_capacity_ = 0;
  std::size_t max_entries_ = 0;
};

}  // namespace cachesim::internal
