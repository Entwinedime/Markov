#pragma once

#include "markov/trace_graph/cost/queries.hpp"
#include "markov/trace_graph/planning/program.hpp"
#include "markov/trace_graph/simulation/execution.hpp"

#include <map>
#include <optional>

namespace markov::trace_graph::simulation {

struct CostGap {
    planning::Work work;
    cost::Response response;
};

struct ProgramResult {
    ExecutionResult execution;
    std::optional<CostGap> cost_gap;
    std::map<cost::Coverage, std::size_t> coverage;
};

// Bridge typed work to the existing clock. Rules never receive cost responses;
// a gap stops this run before committing the incomplete mutation.
class ProgramExecution final : public planning::ProgramRuntime, private EventReceiver {
public:
    ProgramExecution(planning::Program & program, cost::Queries & costs, std::size_t resources);
    ProgramResult run();
    graph::NodeId next_node() const override { return clock_.next_node(); }
    graph::Time now_ns() const override { return clock_.now_ns(); }
    std::optional<graph::NodeId> submit(planning::PlannedMutation mutation) override;

private:
    void on_event(Execution & execution, const Event & event) override;

    planning::Program & program_;
    cost::Queries & costs_;
    Execution clock_;
    std::optional<CostGap> gap_;
    std::map<cost::Coverage, std::size_t> coverage_;
};

} // namespace markov::trace_graph::simulation
