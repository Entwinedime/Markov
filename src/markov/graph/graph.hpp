#pragma once

#include <compare>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "markov/graph/state.hpp"
#include "markov/graph/types.hpp"

namespace markov {

struct GraphPort {
    std::string name;
    Direction direction;
    PortUse use;
    Relation relation;
    std::size_t event;
    std::optional<TensorObservation> tensor;
    std::optional<StreamWait<std::size_t>> wait{};
    std::optional<std::size_t> rendezvous{};
    std::string signal{};
};

struct GraphKvAccess {
    KvAccess value;
    std::size_t start;
    std::size_t finish;
};

struct GraphSlotAccess {
    SlotAccess value;
    std::size_t start;
    std::size_t finish;
};

struct GraphScope {
    ScopeInfo info;
    std::vector<GraphPort> ports;
    std::vector<GraphKvAccess> kv_accesses{};
    std::optional<CommunicationGroup> communication{};
    std::vector<GraphSlotAccess> slot_accesses{};

    const GraphPort & port(std::string_view name, Direction direction) const;
};

// Events are boundaries, not charged operations; dependencies can gate entry or completion.
struct GraphEvent {
    std::size_t scope;
    std::size_t resource;
};

struct GraphWork {
    std::size_t scope;
    std::size_t resource;
    WorkKind kind;
    std::size_t start;
    std::size_t finish;
};

struct GraphEdge {
    std::size_t source;
    std::size_t target;
    Relation kind;

    auto operator<=>(const GraphEdge &) const = default;
};

class CoarseGraph {
public:
    static CoarseGraph read(const nlohmann::json & source);
    nlohmann::json document(bool include_environment = true) const;
    void validate() const;

    // Parent scopes precede their children; shared boundaries belong to their common scope.
    std::size_t common_scope(std::size_t left, std::size_t right) const;
    bool contains(std::size_t scope, std::size_t descendant) const;

    nlohmann::json environment;
    std::vector<Resource> resources;
    std::vector<GraphScope> scopes;
    std::vector<GraphEvent> events;
    std::vector<GraphWork> work;
    std::vector<GraphEdge> edges;
};

} // namespace markov
