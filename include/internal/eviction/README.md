# `include/internal/eviction/`

The seam between the engine and the policies.

| Header | Contents |
|---|---|
| `eviction_policy.hpp` | `PolicyConfig`, `IEvictionPolicy` |
| `policy_registry.hpp` | `PolicyRegistry`, `registerBuiltinPolicies` |

The interface contract — call order, what each hook guarantees, and how to
write a policy — is documented where policy authors will look for it:
[`../../policies/README.md`](../../policies/README.md).

---

## Why this is virtual dispatch

This was the project's biggest design fork, so the reasoning is worth keeping.

A templated `Cache<Policy>` inlines the policy into the hot loop and is
strictly faster per call: no indirect branch, and the optimizer can fuse the
policy's two pointer writes with the surrounding code. It was still rejected,
for two reasons that compound:

1. **A config file cannot select a template argument.** A template parameter is
   resolved during compilation; `policy: s3fifo` in a YAML file is known at
   run time, when the program is already compiled. The usual workaround — know
   the finite set of policies at compile time and pick among the already-
   instantiated types with a runtime switch — works, but the thing it returns
   has to be held in something, and a sweep holding fifteen different policies
   at once needs them to share a type. So a type-erasure layer comes back
   anyway, and once it is there the templating has bought nothing at the
   boundary.
2. **Plugins compose worse.** `Cache<LruPolicy, TlbFilter, AccessBitScanner>`
   makes every combination its own compiled type, so attaching a plugin
   because a config file asked for it means having pre-compiled every
   combination anyone might ever want.

What is actually paid: one indirect call per request, to a target that never
changes for the life of a `Cache` instance. That is a monomorphic call site,
which any modern branch predictor learns in a handful of iterations. The real
cost is that the compiler cannot inline across it — which matters most for the
policies whose work is genuinely two pointer writes, like LRU.

The costs that were actually dominating libCacheSim — an allocation per
cached object, and pointer-chasing through scattered heap memory — are fixed
structurally, by the arena and the intrusive nodes, and they were always the
larger term. Fixing those bought more than switching dispatch mechanisms would
have.

## `PolicyRegistry`

The single point where a runtime string becomes a concrete policy.

```cpp
PolicyRegistry& registry = PolicyRegistry::instance();
auto policy = registry.create("s3fifo", config);     // ConfigError if unknown
registry.add({name, aliases, description, factory}); // add or override
registry.names();                                    // for --list-policies
```

Names are matched with `normalizeKey`, so case and `-`/`_`/space do not
matter, and each entry can declare extra aliases.

An unknown name throws `ConfigError` **listing what is available**, rather
than falling back to a default. A sweep that silently ran LRU for one row
because of a typo in a policy name would be worse than one that refused to
start — the output would look entirely plausible.

A later registration under an existing name replaces the earlier one, so a
user can override a built-in with their own version without touching this
directory.

The registry is a function-local static, which gives thread-safe
initialization and no dependency on the order in which translation units'
globals are constructed. `registerBuiltinPolicies()` runs on first use, so a
library user who only wants the engine never has to call it, and the
`registered` flag is set *before* it runs because it calls `instance()` again
and would otherwise recurse.
