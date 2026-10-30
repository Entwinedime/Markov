#pragma once

#include "markov/trace_graph/cost/queries.hpp"
#include "markov/trace_graph/cost/scaling.hpp"
#include "markov/trace_graph/planning/program.hpp"
#include "markov/trace_graph/simulation/execution.hpp"

#include <map>
#include <optional>
#include <set>
#include <span>
#include <variant>
#include <vector>

namespace markov::trace_graph::simulation {

using CostWork = std::variant<planning::Work, planning::SourceCpuGap, planning::SourceClientDelay, planning::SourceRegionGap>;

struct CostGap {
    CostWork work;
    cost::Response response;
};

struct OperationEvidence {
    std::optional<CostWork> work;
    std::optional<cost::Coverage> coverage;
    Component owner;
    std::optional<Action> completion;
    std::optional<planning::OperationPosition> position;
};

struct ProgramResult {
    ExecutionResult execution;
    std::vector<CostGap> cost_gaps;
    std::map<cost::Coverage, std::size_t> coverage;
    std::vector<std::size_t> scale_matches;
    std::optional<GeneratedGraph> graph;
    std::vector<OperationEvidence> operations;
};

// Bridge typed work to the existing clock. Rules never receive cost responses;
// a gap stops this run before committing the incomplete mutation.
class ProgramExecution final : public planning::ProgramRuntime, private EventReceiver {
public:
    ProgramExecution(planning::Program & program, cost::Queries & costs, std::size_t resources, std::span<const cost::ScalingRule> scaling = {},
                     bool record_graph = false);
    ProgramResult run();
    graph::NodeId next_node() const override { return clock_.next_node(); }
    bool records_graph() const override { return record_graph_; }
    graph::Time now_ns() const override { return clock_.now_ns(); }
    std::optional<graph::NodeId> submit(planning::PlannedMutation mutation) override;
    std::size_t schedule_at(graph::Time time_ns, Component owner, Action action) override { return clock_.schedule_at(time_ns, owner, action); }
    void cancel_timer(std::size_t timer) override { clock_.cancel_timer(timer); }

private:
    void on_event(Execution & execution, const Event & event) override;

    planning::Program & program_;
    cost::Queries & costs_;
    Execution clock_;
    cost::NodeScale scaling_;
    bool record_graph_;
    std::vector<OperationEvidence> operations_;
    std::vector<CostGap> gaps_;

    // Cached response addresses are stable for this cost session. Preserve
    // discovery order in gaps_ while merging repeated missing work points.
    std::set<const cost::Response *> seen_gaps_;
    std::map<cost::Coverage, std::size_t> coverage_;
};

} // namespace markov::trace_graph::simulation
