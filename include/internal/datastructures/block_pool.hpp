#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace cachesim::internal {

// A pointer-stable object pool: allocate() hands out an address that stays
// valid for the lifetime of the pool, and release() returns it for reuse.
//
// Every intrusive data structure here needs exactly this. Nodes are reached
// through CacheEntry::metadata, so their addresses must not move; and a
// simulation does millions of allocate/release pairs per second, so neither
// may touch the system allocator in steady state.
//
// Why not std::deque, which is also pointer-stable: libstdc++ sizes its
// chunks at 512 bytes, which for a 32-byte node is 16 nodes per heap block —
// an allocation every 16 nodes while warming up, and a node stream that is
// only ever contiguous 16 at a time. Here the first block is sized from the
// caller's hint, so the usual case is one block and one allocation, with the
// nodes laid out in one contiguous run.
//
// T must be default-constructible. It is never destroyed before the pool is,
// so T should be trivially destructible (every node type here is) — release()
// is a free-list push, not a destructor call.
template <typename T>
class BlockPool {
 public:
  explicit BlockPool(std::size_t hint = 0) {
    if (hint > 0) {
      addBlock(hint);
      free_list_.reserve(hint);
    }
  }

  BlockPool(const BlockPool&) = delete;
  BlockPool& operator=(const BlockPool&) = delete;
  BlockPool(BlockPool&&) = default;
  BlockPool& operator=(BlockPool&&) = default;

  [[nodiscard]] T* allocate() {
    if (!free_list_.empty()) {
      T* node = free_list_.back();
      free_list_.pop_back();
      return node;
    }
    if (blocks_.empty() || blocks_.back().used == blocks_.back().capacity) {
      std::size_t next = capacity_ / 2;
      if (next < 256) next = 256;
      if (next > (1u << 20)) next = 1u << 20;
      addBlock(next);
    }
    Block& block = blocks_.back();
    return &block.data[block.used++];
  }

  void release(T* node) { free_list_.push_back(node); }

  // Returns every outstanding node to the free list in one step, without
  // walking whatever structure was holding them. Used by clear().
  void releaseAll() {
    free_list_.clear();
    free_list_.reserve(capacity_);
    for (Block& block : blocks_) {
      for (std::size_t i = 0; i < block.used; ++i) {
        free_list_.push_back(&block.data[i]);
      }
    }
  }

  [[nodiscard]] std::size_t capacity() const { return capacity_; }
  [[nodiscard]] std::size_t liveCount() const { return capacity_ - free_list_.size(); }
  [[nodiscard]] std::uint64_t memoryBytes() const {
    return static_cast<std::uint64_t>(capacity_) * sizeof(T) +
           static_cast<std::uint64_t>(free_list_.capacity()) * sizeof(T*);
  }

 private:
  struct Block {
    std::unique_ptr<T[]> data;
    std::size_t capacity = 0;
    std::size_t used = 0;
  };

  void addBlock(std::size_t capacity) {
    Block block;
    block.data = std::make_unique<T[]>(capacity);
    block.capacity = capacity;
    blocks_.push_back(std::move(block));
    capacity_ += capacity;
  }

  std::vector<Block> blocks_;
  std::vector<T*> free_list_;
  std::size_t capacity_ = 0;
};

}  // namespace cachesim::internal
