#pragma once

namespace cachesim::internal {

// The common header of every per-object node that a tracking data structure
// hangs off CacheEntry::metadata.
//
// Why a shared base rather than a plain void*: a CacheEntry only ever has one
// live metadata node, but a policy may own several *different kinds* of
// structure (S3FIFO holds three Queues; a hypothetical size-aware policy
// might hold a Queue and a PriorityQueue). Every structure needs to answer
// "is this entry mine?", and the only way to answer it from a void* is to
// guess the node's type and read a field at a guessed offset — which is
// undefined behaviour the moment the guess is wrong, and happens to produce
// the right answer only as long as every node type keeps its owner pointer at
// the same offset. Giving every node this base makes the question answerable
// from the base pointer alone, with no guessing: read `owner`, compare, and
// only then downcast.
//
// `owner` is the address of the structure that currently tracks the entry, or
// nullptr for a node sitting in a free list.
struct MetadataNode {
  const void* owner = nullptr;
};

}  // namespace cachesim::internal
