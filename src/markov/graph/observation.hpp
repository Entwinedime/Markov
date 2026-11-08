#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "markov/graph/state.hpp"
#include "markov/graph/types.hpp"

namespace markov {

using NativeOrder = std::int64_t;

// Positions are resource-local native order, not timestamps compared across threads or devices.
struct NativeRange {
    NativeOrder begin;
    NativeOrder end;
};

struct NativeResource {
    Resource descriptor;
    bool observed;
    std::vector<NativeRange> payload;

    bool has_payload(NativeOrder begin, NativeOrder end) const;
};

struct NativeSpan {
    std::size_t scope;
    std::size_t resource;
    NativeOrder begin;
    NativeOrder end;
    WorkKind kind;
};

struct NativeAnchor {
    std::size_t span;
    NativeOrder order;

    auto operator<=>(const NativeAnchor &) const = default;
};

struct NativePort {
    std::string name;
    Direction direction;
    PortUse use;
    Relation relation;
    NativeAnchor at;
    std::optional<TensorObservation> tensor;
    std::optional<StreamWait<NativeAnchor>> wait{};
    std::optional<NativeAnchor> rendezvous{};
    std::string signal{};
};

struct NativeKvAccess {
    KvAccess value;
    NativeAnchor start;
    NativeAnchor finish;
};

struct NativeSlotAccess {
    SlotAccess value;
    NativeAnchor start;
    NativeAnchor finish;
};

struct NativeScope {
    ScopeInfo info;
    std::vector<NativePort> ports;
    std::vector<NativeKvAccess> kv_accesses{};
    std::optional<CommunicationGroup> communication{};
    std::vector<NativeSlotAccess> slot_accesses{};
};

struct NativeDependency {
    NativeAnchor source;
    NativeAnchor target;
    Relation kind;
};

struct NativeMeasurements {
    std::string clock;
    std::int64_t window_begin_ns;
    std::int64_t window_end_ns;
    std::map<std::pair<std::size_t, NativeOrder>, std::optional<std::int64_t>> coordinates;
};

// The adapter summarizes native payload presence; no kernel graph or operator ownership table is required.
struct NativeObservation {
    nlohmann::json environment;
    std::vector<NativeResource> resources;
    std::vector<NativeScope> scopes;
    std::vector<NativeSpan> spans;
    std::vector<NativeDependency> dependencies;
    std::optional<NativeMeasurements> measurements;

    static NativeObservation read(const nlohmann::json & source);
};

} // namespace markov
