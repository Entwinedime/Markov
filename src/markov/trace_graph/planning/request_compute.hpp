#pragma once

#include "markov/trace_graph/planning/native_calls.hpp"
#include "markov/trace_graph/planning/program.hpp"
#include "markov/trace_graph/source/observations.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace markov::trace_graph::planning {

enum class ComputeScope { Compute, Request };

struct ControlWork {
    std::vector<Work> work;
    std::vector<AsyncCompletion> completions = {};
};

class RequestControls {
public:
    virtual ~RequestControls() = default;
    virtual FrameInput begin_frame(std::size_t request, std::size_t frame, FrameInput input) = 0;
    // No value means the control awaits a state event; an empty vector is a completed no-op.
    virtual std::optional<ControlWork> begin_result_control(std::size_t request, std::size_t frame, ResultControlPoint control) = 0;
    virtual void end_result_control(ProgramRuntime & runtime, std::size_t request, std::size_t frame, ResultControlPoint control, graph::NodeId completion) = 0;
};

struct ComputeStats {
    struct Preparation {
        std::size_t sequences = 0, operations = 0;
    };
    struct FramePorts {
        source::Index source_frame;
        std::optional<graph::NodeId> entry, submitted, result_ready, completed, callers_entry, callers_completed, result_work_completed, sent;
        std::optional<std::size_t> allocator_view_offset_pages;
    };
    std::size_t collective_services = 0, participant_arrivals = 0;
    std::size_t compute_calls = 0, compute_services = 0;
    std::size_t native_operations = 0, dispatch_delays = 0, source_cpu_gaps = 0;
    std::map<std::string, Preparation> preparations;
    std::vector<FramePorts> frames;
    graph::Time maximum_arrival_span_ns = 0, participant_wait_ns = 0;
    void add(const ComputeStats & other);
};

// Both diagnostic native execution and semantic regions expose the same request ports.
class RequestComputation : public Program {
public:
    static constexpr simulation::Component first_owner = 2;
    void start(ProgramRuntime & runtime) override { start(runtime, std::nullopt); }
    virtual void start(ProgramRuntime & runtime, std::optional<graph::NodeId> cause, std::optional<simulation::Component> previous_owner = std::nullopt) = 0;
    virtual void resume_controls(ProgramRuntime & runtime, graph::NodeId cause) = 0;
    virtual const std::optional<std::string> & issue() const = 0;
    virtual const ComputeStats & stats() const = 0;
    virtual std::optional<graph::NodeId> prefill_result_ready() const = 0;
    virtual std::optional<graph::NodeId> results_ready() const = 0;
    virtual std::optional<graph::NodeId> sent() const = 0;
};

} // namespace markov::trace_graph::planning
