#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "markov/contracts/integer_data.hpp"
#include "markov/execution/protocol.hpp"
#include "markov/graph/access.hpp"

namespace markov {

// Control state is published at region return; tensor readiness is a graph dependency.
struct StateChange {
    std::size_t region;
    std::string resource;
    std::int64_t before;
    std::int64_t after;
    std::optional<std::size_t> value;
};

// Compaction rebinds live values, open calls and pending operations while preserving execution frontiers.
struct GraphRoots {
    std::vector<std::size_t *> values;
    std::vector<std::size_t *> regions;
    std::vector<std::size_t *> operations;
    std::set<std::size_t> storage;
};

// Graph tables have separate identities; execution owners identify which table they reference.
struct RegionGraph {
    std::vector<ValueAccess> values;
    std::vector<nlohmann::json> types;

    std::vector<Operation> operations;
    std::vector<Dependency> dependencies;
    std::vector<nlohmann::json> work;
    std::vector<nlohmann::json> regions;
    std::vector<StateChange> state_changes;
    ExecutionGraph execution;
    std::vector<std::string> execution_resources;

    std::map<std::size_t, std::size_t> external_resources;
    std::map<std::size_t, IntegerData> integer_data;
    std::size_t resource_count = 0;

    // Operation and region identities belong to different tables, including on CPU service ports.
    std::size_t owner_region(ExecutionOwner owner) const;

    // A feedback provider may replace timing, but cannot supply missing waits or operand ports.
    void require_bound_execution() const;

    // References belong to the adapter's live ports and are rebound before old values are released.
    std::vector<std::size_t> retire(GraphRoots roots);
    nlohmann::json document() const;
};

} // namespace markov
