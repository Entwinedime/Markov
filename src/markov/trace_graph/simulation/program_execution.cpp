#include "markov/trace_graph/simulation/program_execution.hpp"

#include <functional>
#include <stdexcept>
#include <utility>
#include <variant>

namespace markov::trace_graph::simulation {

ProgramExecution::ProgramExecution(planning::Program & program, cost::Queries & costs, std::size_t resources)
    : program_(program),
      costs_(costs),
      clock_(resources, *this) {}

std::optional<graph::NodeId> ProgramExecution::submit(planning::PlannedMutation mutation) {
    if (gap_) return std::nullopt;

    Mutation bound{ mutation.owner, {}, std::move(mutation.dependencies) };
    std::map<cost::Coverage, std::size_t> coverage;
    for (const auto & operation : mutation.operations) {
        if (const auto * work = std::get_if<std::reference_wrapper<const planning::Work>>(&operation.cost)) {
            const auto & response = costs_.query(work->get());
            if (!response.duration_ns) {
                gap_ = CostGap{ work->get(), response };
                clock_.stop();
                return std::nullopt;
            }
            ++coverage[*response.coverage];
            bound.operations.push_back({
                { *response.duration_ns, operation.resource, operation.delay ? graph::NodeKind::Delay : graph::NodeKind::Service },
                operation.completion
            });
        }
        else
            bound.operations.push_back({
                { 0, operation.resource, graph::NodeKind::Boundary },
                operation.completion
            });
    }
    const auto begin = clock_.submit(std::move(bound));
    for (const auto & [kind, count] : coverage) coverage_[kind] += count;
    return begin;
}

void ProgramExecution::on_event(Execution &, const Event & event) { program_.on_event(*this, event); }

ProgramResult ProgramExecution::run() {
    program_.start(*this);
    if (!gap_) clock_.run();
    if (!gap_ && !program_.complete()) throw std::runtime_error("No pending event can complete the generated program");

    return { clock_.take_result(), std::move(gap_), std::move(coverage_) };
}

} // namespace markov::trace_graph::simulation
