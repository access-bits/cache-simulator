#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace cachesim {

// Every error this library raises derives from Error, so a caller can catch
// one type and still print something useful. We throw (rather than return
// Status) only at layer boundaries where there is no sensible way to continue:
// a trace file that will not open, a policy name that does not exist, a
// malformed config. Inside the engine, recoverable conditions are reported
// with Status and the caller decides.
class Error : public std::runtime_error {
 public:
  explicit Error(const std::string& what) : std::runtime_error(what) {}
};

// A config file, CLI argument, or parameter string the user can fix.
class ConfigError : public Error {
 public:
  explicit ConfigError(const std::string& what) : Error("config error: " + what) {}
};

// A trace file that is missing, unreadable, truncated, or not in the format
// it was declared to be in.
class TraceError : public Error {
 public:
  explicit TraceError(const std::string& what) : Error("trace error: " + what) {}
};

// An invariant inside the engine was violated — a policy returned no victim
// while the cache was over capacity, a data structure reported a failure it
// should not be able to report. These are bugs (ours or a policy's), not user
// error, and the message says which component reported it.
class EngineError : public Error {
 public:
  explicit EngineError(const std::string& what) : Error("engine error: " + what) {}
};

}  // namespace cachesim
