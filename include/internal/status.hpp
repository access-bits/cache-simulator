#pragma once

namespace cachesim {

// Lightweight pass/fail signal for operations that can meaningfully fail
// (bad usage, exhausted capacity, inconsistent state) without needing an
// exception at the point of failure. Callers decide whether a kFailure is
// recoverable or should be escalated (e.g. Cache turns internal failures
// into thrown exceptions, since it has no safe way to continue).
enum class Status { kSuccess, kFailure };

}  // namespace cachesim
