#pragma once

#include <cstdint>

namespace cachesim {

// A single trace event: which object, how big, and when.
// Deliberately minimal — hardware/plugin-specific tags live in a side channel, not here.
struct Request {
  std::uint64_t obj_id;
  std::uint32_t size;
  std::int64_t timestamp;
};

}  // namespace cachesim
