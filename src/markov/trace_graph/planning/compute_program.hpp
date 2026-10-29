#pragma once

#include "markov/trace_graph/planning/compute_plan.hpp"
#include "markov/trace_graph/planning/native_calls.hpp"
#include "markov/trace_graph/planning/program.hpp"
#include "markov/trace_graph/source/context.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace markov::trace_graph::planning {

struct FrameCompute {
    source::Index frame;
    FrameInput variables;
    ComputePlan plan;
};

enum class ComputeScope { Compute, Request };

class ResultControls {
public:
    virtual ~ResultControls() = default;
    // Adapters expand logical cache effects before the runtime queries costs.
    virtual std::vector<Work> begin_result_control(std::size_t request, ResultControl control) = 0;
    virtual void end_result_control(ProgramRuntime & runtime, std::size_t request, ResultControl control, graph::NodeId completion) = 0;
};

struct ComputeStats {
    struct Preparation {
        std::size_t sequences = 0, operations = 0;
    };
    struct FramePorts {
        source::Index source_frame;
        std::optional<graph::NodeId> entry, submitted, result_ready, completed, callers_entry, callers_completed, result_work_completed, sent;
    };
    std::size_t collective_services = 0, participant_arrivals = 0;
    std::size_t compute_calls = 0, compute_services = 0;
    std::size_t native_operations = 0, dispatch_delays = 0, source_cpu_gaps = 0;
    std::map<std::string, Preparation> preparations;
    std::vector<FramePorts> frames;
    graph::Time maximum_arrival_span_ns = 0, participant_wait_ns = 0;
    void add(const ComputeStats & other);
};

// Native blocking tensor collectives fence the known work on each participant.
// Arrivals, the shared completion service and subsequent compute use one clock.
// Observed overlap releases the next caller while retaining queue dependencies.
class ComputeProgram final : public Program {
public:
    static constexpr simulation::Component first_owner = 2;
    ComputeProgram(const source::SourceContext & source, std::vector<FrameCompute> frames, std::size_t first_resource, PreparationCache & preparations,
                   ExecutionQueues & queues, ComputeScope scope = ComputeScope::Compute, simulation::Component owner = first_owner,
                   ResultControls * controls = nullptr, std::size_t request = 0);
    ComputeProgram(const ComputeProgram &) = delete;

    void start(ProgramRuntime & runtime, std::optional<graph::NodeId> cause, std::optional<simulation::Component> previous_owner = std::nullopt);
    void start(ProgramRuntime & runtime) override { start(runtime, std::nullopt); }
    void on_event(ProgramRuntime & runtime, const simulation::Event & event) override;
    bool complete() const override { return complete_ || issue_.has_value(); }
    std::size_t resources() const { return first_resource_ + source_.observations().collective_groups.size(); }
    const std::optional<std::string> & issue() const { return issue_; }
    const ComputeStats & stats() const { return stats_; }
    std::optional<graph::NodeId> prefill_result_ready() const { return prefill_result_ready_; }
    std::optional<graph::NodeId> results_ready() const { return results_ready_; }
    std::optional<graph::NodeId> sent() const { return sent_; }

private:
    enum class ActionKind : std::uint32_t {
        ForwardGranted,
        CallersGranted,
        SegmentDone,
        CollectiveDone,
        FrameSubmitted,
        ResultReady,
        ResultStepSubmitted,
        ResultStepDone,
        ResultWorkDone,
        Sent,
        PrefillReady,
        ResultsReady,
        Complete
    };
    static simulation::Action action(ActionKind kind, std::size_t subject) { return { std::to_underlying(kind), subject }; }

    struct Segment {
        NativePlan plan;
        std::optional<std::size_t> collective;
        std::size_t participant = 0;
        ComputeStats stats;
    };
    struct Frame {
        std::vector<Segment> segments;
        FrameInput input{};
        Phase phase = Phase::Prefill;
        std::vector<std::size_t> results;
        std::vector<graph::NodeId> result_completions;
        std::size_t result_at = 0;
        std::size_t at = 0;
        std::size_t caller = 0;
        std::optional<graph::NodeId> result_producer;
        std::optional<std::size_t> previous, next;
        bool overlap = false;
        bool publishes_prefill_prefix = false;
        bool callers_modeled = false, callers_requested = false, next_started = false, finished = false;
    };
    struct ResultStep {
        std::size_t frame;
        source::Index sequence;
        CallResources resources;
        NativePlan plan;
    };
    struct Arrival {
        graph::Time time_ns;
        graph::NodeId node;
    };
    struct Collective {
        Work work;
        source::Index group;
        std::vector<std::optional<std::size_t>> peers;
        std::vector<std::optional<Arrival>> arrivals;
    };

    void segment(ProgramRuntime & runtime, std::size_t frame, std::optional<graph::NodeId> cause);
    bool append_compute(const FrameCompute & frame, source::Index source_call, std::size_t & at, CallResources resources, Segment & segment);
    void arrive(ProgramRuntime & runtime, std::size_t frame, const simulation::Event & event);
    void join_prefill_results(ProgramRuntime & runtime);
    void start_next(ProgramRuntime & runtime, std::size_t frame);
    void process_result(ProgramRuntime & runtime, std::size_t frame);
    void run_result(ProgramRuntime & runtime, std::size_t frame, graph::NodeId granted);
    void result_step_submitted(ProgramRuntime & runtime, const simulation::Event & event);
    void result_step_done(ProgramRuntime & runtime, const simulation::Event & event);
    void launch_frame(ProgramRuntime & runtime, std::size_t frame, std::optional<graph::NodeId> cause);
    void join_results(ProgramRuntime & runtime);
    void finish_frame(ProgramRuntime & runtime, std::size_t frame);

    const source::SourceContext & source_;
    PreparationCache & preparations_;
    ExecutionQueues & queues_;
    simulation::Component owner_;
    ResultControls * controls_;
    std::size_t request_;
    std::size_t first_resource_;
    std::vector<Frame> frames_;
    std::vector<ResultStep> result_steps_;
    std::vector<Collective> collectives_;
    std::vector<std::size_t> roots_;
    std::vector<graph::NodeId> completed_;
    std::size_t finished_frames_ = 0;
    std::optional<std::string> issue_;
    ComputeStats stats_;
    std::optional<graph::NodeId> prefill_result_ready_;
    std::optional<graph::NodeId> results_ready_, sent_;
    bool prefill_join_submitted_ = false;
    bool results_join_submitted_ = false;
    bool complete_ = false;
};

} // namespace markov::trace_graph::planning
