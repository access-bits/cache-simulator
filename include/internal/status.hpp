#pragma once

namespace cachesim {

// Lightweight pass/fail signal for operations that can meaningfully fail
// (bad usage, exhausted capacity, inconsistent state) without needing an
// exception at the point of failure. Callers decide whether a kFailure is
// recoverable or should be escalated — Cache, for instance, turns an internal
// failure into a thrown EngineError, because once a policy and the cache
// structure disagree there is no safe way to keep replaying.
enum class Status { kSuccess, kFailure };

[[nodiscard]] inline bool ok(Status s) { return s == Status::kSuccess; }

}  // namespace cachesim
