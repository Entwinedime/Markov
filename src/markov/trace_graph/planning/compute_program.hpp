#pragma once

#include "markov/trace_graph/planning/compute_plan.hpp"
#include "markov/trace_graph/planning/native_calls.hpp"
#include "markov/trace_graph/planning/program.hpp"
#include "markov/trace_graph/source/context.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace markov::trace_graph::planning {

struct FrameCompute {
    source::Index frame;
    AttentionInput variables;
    ComputePlan plan;
};

struct ComputeStats {
    std::size_t collective_services = 0, participant_arrivals = 0, queue_dependencies = 0;
    std::size_t metadata_sequences = 0, native_operations = 0, dispatch_delays = 0;
    graph::Time maximum_arrival_span_ns = 0, participant_wait_ns = 0;
    void add(const ComputeStats & other);
};

// Native blocking tensor collectives fence the known work on each participant.
// Arrivals, the shared completion service and subsequent compute use one clock.
class ComputeProgram final : public Program {
public:
    static constexpr simulation::Component owner = 2;
    ComputeProgram(const source::SourceContext & source, std::vector<FrameCompute> frames, std::size_t first_resource);
    ComputeProgram(const ComputeProgram &) = delete;

    void start(ProgramRuntime & runtime, std::optional<graph::NodeId> cause);
    void start(ProgramRuntime & runtime) override { start(runtime, std::nullopt); }
    void on_event(ProgramRuntime & runtime, const simulation::Event & event) override;
    bool complete() const override { return complete_ || issue_.has_value(); }
    std::size_t resources() const { return first_resource_ + source_.observations().collective_groups.size(); }
    const std::optional<std::string> & issue() const { return issue_; }
    const ComputeStats & stats() const { return stats_; }

private:
    enum class ActionKind : std::uint32_t { SegmentDone, CollectiveDone, MetadataDone, Complete };
    static simulation::Action action(ActionKind kind, std::size_t subject) { return { std::to_underlying(kind), subject }; }

    struct Segment {
        std::size_t first, last;
        std::optional<std::size_t> collective;
        std::size_t participant;
        std::optional<std::size_t> metadata = std::nullopt;
    };
    struct Frame {
        FrameCompute compute;
        std::vector<Segment> segments;
        std::size_t at = 0;
        std::optional<std::size_t> next;
        std::vector<NativePlan> metadata;
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
    void arrive(ProgramRuntime & runtime, std::size_t frame, const simulation::Event & event);

    const source::SourceContext & source_;
    std::size_t first_resource_;
    std::vector<Frame> frames_;
    std::vector<Collective> collectives_;
    std::vector<std::size_t> roots_;
    std::vector<graph::NodeId> completed_;
    std::optional<std::string> issue_;
    ComputeStats stats_;
    bool complete_ = false;
};

} // namespace markov::trace_graph::planning
