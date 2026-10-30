#pragma once

#include "markov/trace_graph/planning/request_compute.hpp"
#include "markov/trace_graph/source/context.hpp"

#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace markov::trace_graph::planning {

struct RegionBackend {
    std::string implementation;
    std::string result_implementation;
    std::size_t maximum_prefill_tokens;
};

struct RegionFrame {
    source::Index source_frame;
    FrameInput input;
};

// These are observable invocation conditions, not a replica of a compiler's
// internal cache. Independent responses retain that initialization uncertainty.
class RegionPreparation {
public:
    explicit RegionPreparation(std::optional<KernelCacheInitial> initial) : initial_(initial) {}
    std::optional<KernelCacheInitial> initial() const { return initial_; }
    Features enter(std::size_t caller, Phase phase, const FrameInput & input);

private:
    std::optional<KernelCacheInitial> initial_;
    std::set<std::pair<std::size_t, Phase>> phases_;
    std::set<std::tuple<std::size_t, Phase, double, double, double>> inputs_;
};

// A semantic frame has separate caller and stream progress ports. The same
// runtime executes them in parallel; result CPU waits for the group's copies.
class RegionCompute final : public RequestComputation {
public:
    RegionCompute(const source::SourceContext & source, std::vector<RegionFrame> frames, RegionBackend backend, std::size_t first_resource,
                  RegionPreparation & preparation, ExecutionQueues & queues, RequestControls & controls, std::size_t request);
    void start(ProgramRuntime & runtime) override { start(runtime, std::nullopt); }
    void start(ProgramRuntime & runtime, std::optional<graph::NodeId> cause, std::optional<simulation::Component> previous_owner = std::nullopt) override;
    void on_event(ProgramRuntime & runtime, const simulation::Event & event) override;
    void resume_controls(ProgramRuntime &, graph::NodeId) override {}
    bool complete() const override { return complete_ || issue_.has_value(); }
    const std::optional<std::string> & issue() const override { return issue_; }
    const ComputeStats & stats() const override { return stats_; }
    std::optional<graph::NodeId> prefill_result_ready() const override { return prefill_result_ready_; }
    std::optional<graph::NodeId> results_ready() const override { return results_ready_; }
    std::optional<graph::NodeId> sent() const override { return sent_; }

private:
    enum class ActionKind : std::uint32_t {
        Granted,
        Prepared,
        Entered,
        Submitted,
        Ready,
        ResultGranted,
        SentFrame,
        ResultDone,
        PrefillReady,
        ResultsReady,
        Sent,
        Done
    };
    static simulation::Action action(ActionKind kind, std::size_t subject) { return { std::to_underlying(kind), subject }; }
    struct Frame {
        RegionFrame description;
        std::size_t caller, rank, group;
        Phase phase;
        std::optional<std::size_t> previous, next;
        std::optional<graph::NodeId> entered, input_ready;
        Features features;
        Work prepare_cpu, prepare_stream, prepare_return, batch_cpu, batch_stream, batch_return, result_cpu, result_tail;
        bool overlap, has_result, publishes, sends;
        bool next_started = false, result_requested = false;
    };
    struct Group {
        std::vector<std::size_t> frames;
        bool launched = false;
    };
    void prepare(ProgramRuntime & runtime, std::size_t frame, graph::NodeId cause);
    void run_frame(ProgramRuntime & runtime, std::size_t frame, graph::NodeId cause);
    void launch_group(ProgramRuntime & runtime, std::size_t group);
    void advance(ProgramRuntime & runtime, std::size_t frame);
    void process_result(ProgramRuntime & runtime, std::size_t frame, graph::NodeId cause);
    void finish(ProgramRuntime & runtime);
    graph::NodeId join(ProgramRuntime & runtime, std::span<const graph::NodeId> causes, ActionKind action);

    const source::SourceContext & source_;
    RegionBackend backend_;
    std::size_t first_resource_, request_;
    simulation::Component owner_;
    RegionPreparation & preparation_;
    ExecutionQueues & queues_;
    RequestControls & controls_;
    std::vector<Frame> frames_;
    std::vector<Group> groups_;
    std::vector<std::size_t> roots_;
    ComputeStats stats_;
    std::optional<std::string> issue_;
    std::optional<graph::NodeId> prefill_result_ready_, results_ready_, sent_;
    bool prefill_joined_ = false, results_joined_ = false, sent_joined_ = false, done_joined_ = false, complete_ = false;
};

} // namespace markov::trace_graph::planning
