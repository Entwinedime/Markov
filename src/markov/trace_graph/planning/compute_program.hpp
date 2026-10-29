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

struct ComputeStats {
    struct Preparation {
        std::size_t sequences = 0, operations = 0;
    };
    struct FramePorts {
        source::Index source_frame;
        std::optional<graph::NodeId> entry, submitted, result_ready, completed, callers_entry, callers_completed, sent;
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
    static constexpr simulation::Component owner = 2;
    ComputeProgram(const source::SourceContext & source, std::vector<FrameCompute> frames, std::size_t first_resource, PreparationCache & preparations);
    ComputeProgram(const ComputeProgram &) = delete;

    void start(ProgramRuntime & runtime, std::optional<graph::NodeId> cause);
    void start(ProgramRuntime & runtime) override { start(runtime, std::nullopt); }
    void on_event(ProgramRuntime & runtime, const simulation::Event & event) override;
    bool complete() const override { return complete_ || issue_.has_value(); }
    std::size_t resources() const { return first_resource_ + source_.observations().collective_groups.size(); }
    const std::optional<std::string> & issue() const { return issue_; }
    const ComputeStats & stats() const { return stats_; }
    std::optional<graph::NodeId> prefill_result_ready() const { return prefill_result_ready_; }

private:
    enum class ActionKind : std::uint32_t { SegmentDone, CollectiveDone, FrameSubmitted, ResultReady, CallersDone, Sent, PrefillReady, Complete };
    static simulation::Action action(ActionKind kind, std::size_t subject) { return { std::to_underlying(kind), subject }; }

    struct Segment {
        NativePlan plan;
        std::optional<std::size_t> collective;
        std::size_t participant = 0;
        ComputeStats stats;
    };
    struct Frame {
        std::vector<Segment> segments;
        NativePlan result_callers;
        ComputeStats caller_stats;
        std::size_t at = 0;
        std::optional<std::size_t> previous, next;
        bool overlap = false;
        bool publishes_prefill_prefix = false;
        bool callers_modeled = false, callers_started = false, next_started = false, finished = false;
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
    void finish_frame(ProgramRuntime & runtime, std::size_t frame);

    const source::SourceContext & source_;
    PreparationCache & preparations_;
    std::size_t first_resource_;
    std::vector<Frame> frames_;
    std::vector<Collective> collectives_;
    std::vector<std::size_t> roots_;
    std::vector<graph::NodeId> completed_;
    std::size_t finished_frames_ = 0;
    NativeFrontier frontier_;
    std::optional<std::string> issue_;
    ComputeStats stats_;
    std::optional<graph::NodeId> prefill_result_ready_;
    bool prefill_join_submitted_ = false;
    bool complete_ = false;
};

} // namespace markov::trace_graph::planning
