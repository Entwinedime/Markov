#pragma once

#include "markov/trace_graph/graph/graph.hpp"
#include "markov/trace_graph/planning/work.hpp"
#include "markov/trace_graph/simulation/events.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <variant>
#include <vector>

namespace markov::trace_graph::planning {

struct Boundary {};

// A retained client interval is an explicit residual approximation, not a
// migrated CPU service or a new cost-model feature.
// shortcut: same request and frontend deployment; replace when transport work changes.
struct SourceDelay {
    graph::NodeId node;
};

struct PlannedOperation {
    std::size_t resource;

    // Work references live through the synchronous submit call. A boundary is
    // explicitly cost-free; missing work is never represented by a null pointer.
    std::variant<Boundary, std::reference_wrapper<const Work>, SourceDelay> cost;
    std::optional<simulation::Action> completion = std::nullopt;
    bool delay = false; // Readiness latency; it does not occupy a service resource.
    std::optional<std::size_t> source_operation = std::nullopt;
};

struct PlannedMutation {
    simulation::Component owner;
    std::vector<PlannedOperation> operations;
    std::vector<graph::Edge> dependencies;
};

class ProgramRuntime {
public:
    virtual ~ProgramRuntime() = default;
    virtual graph::NodeId next_node() const = 0;
    virtual graph::Time now_ns() const = 0;
    virtual std::optional<graph::NodeId> submit(PlannedMutation mutation) = 0;
    virtual std::size_t schedule_at(graph::Time time_ns, simulation::Component owner, simulation::Action action) = 0;
    virtual void cancel_timer(std::size_t timer) = 0;
};

class Program {
public:
    virtual ~Program() = default;
    virtual void start(ProgramRuntime & runtime) = 0;
    virtual void on_event(ProgramRuntime & runtime, const simulation::Event & event) = 0;
    virtual bool complete() const = 0;
};

} // namespace markov::trace_graph::planning
