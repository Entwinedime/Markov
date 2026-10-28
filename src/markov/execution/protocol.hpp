#pragma once

#include <compare>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "markov/graph/access.hpp"

namespace markov {

enum class ExecutionKind {
    Enqueue,
    Submit,
    Device,
    Copy,
    CopyAsync,
    WaitStream,
    WaitDevice,
    Transfer,
    QueueReturn,
    Unclassified,
    Host,
    WaitEntry,
    RecordEvent,
    WaitEvent,
    WaitEventComplete,
    Barrier,
    Communication
};

ExecutionKind execution_kind(std::string_view name);
std::string_view execution_name(ExecutionKind kind);

struct ExecutionStep {
    ExecutionKind kind;
    std::size_t resource;
    std::optional<std::size_t> target;
    std::string name;
    std::optional<std::size_t> part = std::nullopt;
};

struct ExecutionRequirement {
    std::size_t resource;
    std::optional<std::size_t> source;
    std::size_t target;
    std::string kind;
};

struct ExecutionSignal {
    std::size_t resource;
    std::size_t source;
};

// Entry is the first work's start; return is the final work's finish.
struct ExecutionPorts {
    std::size_t entry;
    std::size_t returned;
};

struct ExecutionOwner {
    enum class Kind { Operation, Region };

    Kind kind;
    std::size_t identity;

    auto operator<=>(const ExecutionOwner &) const = default;
    void rebase(std::size_t operation_offset, std::size_t region_offset);
    const char * key() const;
};

struct ExecutionNode {
    ExecutionStep step;
    ExecutionOwner owner;
};

// Runtime order and data readiness remain separate until target storage bindings are known.
struct ExecutionPattern {
    std::vector<ExecutionStep> steps;
    std::vector<Dependency> links;
    std::vector<std::vector<std::size_t>> sequences;
    std::vector<ExecutionRequirement> requirements;
    std::vector<ExecutionSignal> signals;
    std::optional<ExecutionPorts> host_call;
    std::optional<ExecutionPorts> communication;

    static ExecutionPattern read(const nlohmann::json & source);
};

// Resource frontiers connect fragments; owner identities connect execution back to logical work.
class ExecutionGraph {
public:
    void append(const ExecutionPattern & pattern, std::span<const std::size_t> resources, std::size_t operation, std::size_t region);
    void append(ExecutionGraph fragment, std::size_t operation_offset, std::size_t region_offset, std::size_t resource_offset = 0);
    void append_host(std::size_t region, std::size_t resource);
    void initially_idle(std::size_t resource);
    void depend(std::size_t source, std::size_t target, std::string kind);
    std::vector<std::size_t> initial_nodes(std::size_t begin) const;
    std::vector<std::size_t> terminal_nodes(std::size_t begin) const;
    void connect_communication(std::span<const std::size_t> operations, std::size_t resource, std::size_t region, const std::string & kind);
    void connect_accesses(std::span<const ValueAccess> values, std::span<const nlohmann::json> types, std::span<const Operation> operations);

    // Release historical IR, retaining state readiness and possibly unfinished execution frontiers.
    std::vector<std::size_t> retire(std::span<const ValueAccess> values, const std::map<std::size_t, std::size_t> & retained_values,
                                    const std::set<std::size_t> & retained_storage, const std::map<std::size_t, std::size_t> & retained_regions);

    std::size_t first_node() const;
    std::span<const ExecutionNode> nodes() const;
    std::span<const Dependency> dependencies() const;
    ExecutionPorts region_call(std::size_t region) const;
    std::span<const ExecutionRequirement> incoming_waits() const;
    bool has_unbound_accesses(std::size_t operation_count) const;
    bool has_unbound_communications() const;
    nlohmann::json document() const;

private:
    struct StorageFrontier {
        std::map<std::size_t, std::size_t> writers;
        std::map<std::size_t, std::size_t> readers;
    };

    void sequence(std::size_t node, std::size_t resource);
    void call(ExecutionOwner owner, ExecutionPorts ports);

    std::vector<ExecutionNode> nodes_;
    std::vector<Dependency> dependencies_;
    std::size_t first_node_ = 0;

    std::map<std::size_t, std::size_t> heads_;
    std::map<std::size_t, std::size_t> tails_;
    std::set<std::size_t> initially_idle_;
    std::vector<ExecutionRequirement> incoming_;
    std::map<ExecutionOwner, ExecutionPorts> host_calls_;
    std::map<std::size_t, std::map<std::size_t, ExecutionPorts>> data_ports_;
    std::map<std::size_t, ExecutionPorts> communications_;

    std::map<std::size_t, StorageFrontier> storage_frontiers_;
    std::map<std::size_t, std::size_t> value_ready_;
    std::size_t bound_operations_ = 0;
    bool missing_call_ports_ = false;
};

} // namespace markov
