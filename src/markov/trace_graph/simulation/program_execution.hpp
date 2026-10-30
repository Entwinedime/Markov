#pragma once

#include "markov/trace_graph/cost/queries.hpp"
#include "markov/trace_graph/planning/program.hpp"
#include "markov/trace_graph/simulation/execution.hpp"

#include <map>
#include <optional>
#include <set>
#include <variant>
#include <vector>

namespace markov::trace_graph::simulation {

struct CostGap {
    std::variant<planning::Work, planning::SourceCpuGap, planning::SourceClientDelay, planning::SourceRegionGap> work;
    cost::Response response;
};

struct ProgramResult {
    ExecutionResult execution;
    std::vector<CostGap> cost_gaps;
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
    std::size_t schedule_at(graph::Time time_ns, Component owner, Action action) override { return clock_.schedule_at(time_ns, owner, action); }
    void cancel_timer(std::size_t timer) override { clock_.cancel_timer(timer); }

private:
    void on_event(Execution & execution, const Event & event) override;

    planning::Program & program_;
    cost::Queries & costs_;
    Execution clock_;
    std::vector<CostGap> gaps_;

    // Cached response addresses are stable for this cost session. Preserve
    // discovery order in gaps_ while merging repeated missing work points.
    std::set<const cost::Response *> seen_gaps_;
    std::map<cost::Coverage, std::size_t> coverage_;
};

} // namespace markov::trace_graph::simulation
