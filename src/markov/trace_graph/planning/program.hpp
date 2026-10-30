#pragma once

#include "markov/trace_graph/graph/graph.hpp"
#include "markov/trace_graph/planning/work.hpp"
#include "markov/trace_graph/simulation/events.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

namespace markov::trace_graph::planning {

struct Boundary {};

struct SourceRegionGap {
    std::size_t interval;
};

// Transport tails are referenced directly by request, without materializing a
// detailed source graph. These intervals remain explicit residual estimates.
struct SourceClientDelay {
    std::size_t request;
    bool response;
};

// A source inter-call interval is a residual approximation, not proven service.
// shortcut: preserve the same caller recipe; replace when host work changes.
struct SourceCpuGap {
    std::size_t interval;
};

// Diagnostic address in the generated execution, separate from cost features.
// Port names are static literals; an unknown participant stays unqualified.
struct OperationPosition {
    std::optional<std::size_t> request;
    std::size_t iteration;
    std::optional<std::size_t> participant;
    std::string_view port;

    // Full logical token ancestry through the last page of this operation.
    std::optional<std::vector<std::int64_t>> data = std::nullopt;
};

struct PlannedOperation {
    std::size_t resource;

    // Work references live through the synchronous submit call. A boundary is
    // explicitly cost-free; missing work is never represented by a null pointer.
    std::variant<Boundary, std::reference_wrapper<const Work>, SourceRegionGap, SourceClientDelay, SourceCpuGap> cost;
    std::optional<simulation::Action> completion = std::nullopt;
    bool delay = false; // Readiness latency; it does not occupy a service resource.
    std::optional<std::size_t> source_operation = std::nullopt;
    std::optional<std::size_t> source_native_work = std::nullopt;
    std::optional<simulation::Component> owner = std::nullopt;
    std::optional<OperationPosition> position = std::nullopt;
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
    virtual bool records_graph() const = 0;
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
