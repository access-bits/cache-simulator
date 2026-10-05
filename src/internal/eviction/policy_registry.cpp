#include "internal/eviction/policy_registry.hpp"

#include <algorithm>

#include "internal/common/error.hpp"
#include "internal/common/string_util.hpp"
#include "policies/arc.hpp"
#include "policies/belady.hpp"
#include "policies/clock.hpp"
#include "policies/lfu.hpp"
#include "policies/lfuda.hpp"
#include "policies/lru.hpp"
#include "policies/lru_k.hpp"
#include "policies/random.hpp"
#include "policies/s3fifo.hpp"
#include "policies/sieve.hpp"
#include "policies/slru.hpp"
#include "policies/two_q.hpp"
#include "policies/wtinylfu.hpp"

namespace cachesim {
namespace {

template <typename Policy>
PolicyRegistry::Factory make() {
  return [](const PolicyConfig& config) -> std::unique_ptr<IEvictionPolicy> {
    return std::make_unique<Policy>(config);
  };
}

}  // namespace

PolicyRegistry& PolicyRegistry::instance() {
  // Function-local statics: thread-safe initialization, and no dependency on
  // the order in which translation units' globals are constructed. The
  // `registered` flag is set before registerBuiltinPolicies() runs, because
  // that function calls instance() again and would otherwise recurse.
  static PolicyRegistry registry;
  static bool registered = false;
  if (!registered) {
    registered = true;
    registerBuiltinPolicies();
  }
  return registry;
}

void PolicyRegistry::add(Entry entry) {
  // A later registration under the same name replaces the earlier one, so a
  // user can override a built-in policy with their own version.
  const std::string key = normalizeKey(entry.name);
  const auto existing = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& e) {
    return normalizeKey(e.name) == key;
  });
  if (existing != entries_.end()) {
    *existing = std::move(entry);
    return;
  }
  entries_.push_back(std::move(entry));
}

const PolicyRegistry::Entry* PolicyRegistry::find(std::string_view name) const {
  const std::string key = normalizeKey(name);
  for (const Entry& entry : entries_) {
    if (normalizeKey(entry.name) == key) return &entry;
    for (const std::string& alias : entry.aliases) {
      if (normalizeKey(alias) == key) return &entry;
    }
  }
  return nullptr;
}

bool PolicyRegistry::contains(std::string_view name) const { return find(name) != nullptr; }

std::unique_ptr<IEvictionPolicy> PolicyRegistry::create(std::string_view name,
                                                        const PolicyConfig& config) const {
  const Entry* entry = find(name);
  if (entry == nullptr) {
    std::string available;
    for (const Entry& candidate : entries_) {
      if (!available.empty()) available += ", ";
      available += candidate.name;
    }
    throw ConfigError("unknown eviction policy '" + std::string(name) + "'; available: " +
                      available);
  }
  return entry->factory(config);
}

std::vector<std::string> PolicyRegistry::names() const {
  std::vector<std::string> out;
  out.reserve(entries_.size());
  for (const Entry& entry : entries_) out.push_back(entry.name);
  return out;
}

void registerBuiltinPolicies() {
  PolicyRegistry& registry = PolicyRegistry::instance();
  registry.add({"LRU", {"lru"}, "Least recently used", make<policies::Lru>()});
  registry.add({"FIFO", {"fifo"}, "First in, first out", make<policies::Fifo>()});
  registry.add({"MRU", {"mru"}, "Most recently used (diagnostic baseline)",
                make<policies::Mru>()});
  registry.add({"Random", {"rand"}, "Uniformly random victim", make<policies::Random>()});
  registry.add({"Clock", {"second-chance", "sc"},
                "CLOCK / second-chance FIFO; n-bit-counter=K", make<policies::Clock>()});
  registry.add({"Sieve", {}, "SIEVE (NSDI'24): lazy promotion, quick demotion",
                make<policies::Sieve>()});
  registry.add({"LFU", {}, "Exact least frequently used, FIFO tie-break",
                make<policies::Lfu>()});
  registry.add({"LFU-DA", {"lfuda"}, "LFU with dynamic aging", make<policies::LfuDa>()});
  registry.add({"SLRU", {"s4lru", "segmented-lru"},
                "Segmented LRU; segments=N, fractions=a|b|c|d", make<policies::Slru>()});
  registry.add({"2Q", {"two-q"}, "2Q (VLDB'94); kin=f, kout=f", make<policies::TwoQ>()});
  registry.add({"ARC", {}, "Adaptive Replacement Cache (FAST'03)", make<policies::Arc>()});
  registry.add({"S3FIFO", {"s3-fifo"},
                "S3-FIFO (SOSP'23); small-size-ratio=f, move-to-main-threshold=k",
                make<policies::S3Fifo>()});
  registry.add({"LRU-K", {"lru2"}, "LRU-K (SIGMOD'93); k=K", make<policies::LruK>()});
  registry.add({"W-TinyLFU", {"tinylfu"},
                "Window TinyLFU with a count-min sketch admission filter; window-ratio=f",
                make<policies::WTinyLfu>()});
  registry.add({"Belady", {"opt", "min"},
                "Offline optimal; requires an oracle trace", make<policies::Belady>()});
}

}  // namespace cachesim
