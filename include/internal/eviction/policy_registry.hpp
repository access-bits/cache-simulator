#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "internal/eviction/eviction_policy.hpp"

namespace cachesim {

// Name -> policy factory. This is the one place where a runtime string from a
// config file turns into a concrete policy, and it is the reason the engine
// uses virtual dispatch at all: a YAML file naming "s3fifo" cannot select a
// template argument, and a sweep holding fifteen different policies at once
// needs them to share a type.
//
// Everything downstream of this call is monomorphic — one Cache instance runs
// one policy for its whole life, so the call site is a single-target indirect
// branch that any modern predictor learns immediately.
class PolicyRegistry {
 public:
  using Factory = std::function<std::unique_ptr<IEvictionPolicy>(const PolicyConfig&)>;

  struct Entry {
    std::string name;                 // canonical name, as reported in results
    std::vector<std::string> aliases; // spellings accepted from a config
    std::string description;
    Factory factory;
  };

  static PolicyRegistry& instance();

  void add(Entry entry);

  // Builds the named policy. Names are matched case-insensitively and
  // ignoring '-'/'_'/' ', so "S3FIFO", "s3fifo" and "s3-fifo" are one
  // policy. Throws ConfigError, listing what is available, if the name is
  // unknown — a sweep that silently fell back to LRU because of a typo in one
  // row would be worse than one that refused to start.
  [[nodiscard]] std::unique_ptr<IEvictionPolicy> create(std::string_view name,
                                                        const PolicyConfig& config) const;

  [[nodiscard]] bool contains(std::string_view name) const;

  [[nodiscard]] const std::vector<Entry>& entries() const { return entries_; }

  // Canonical names, in registration order; for --list-policies and for
  // error messages.
  [[nodiscard]] std::vector<std::string> names() const;

 private:
  PolicyRegistry() = default;

  [[nodiscard]] const Entry* find(std::string_view name) const;

  std::vector<Entry> entries_;
};

// Registers every policy that ships with the simulator. Called automatically
// the first time the registry is used, so a library user who only wants the
// engine does not have to know about it, and a user adding their own policy
// calls PolicyRegistry::instance().add(...) afterwards.
void registerBuiltinPolicies();

}  // namespace cachesim
