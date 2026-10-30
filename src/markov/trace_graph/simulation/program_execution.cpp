#include "markov/trace_graph/simulation/program_execution.hpp"

#include <functional>
#include <iterator>
#include <stdexcept>
#include <utility>
#include <variant>

namespace markov::trace_graph::simulation {

ProgramExecution::ProgramExecution(planning::Program & program, cost::Queries & costs, std::size_t resources, std::span<const cost::ScalingRule> scaling,
                                   bool record_graph)
    : program_(program),
      costs_(costs),
      clock_(resources, *this, record_graph),
      scaling_(scaling),
      record_graph_(record_graph) {}

std::optional<graph::NodeId> ProgramExecution::submit(planning::PlannedMutation mutation) {
    if (!gaps_.empty()) return std::nullopt;

    Mutation bound{ mutation.owner, {}, std::move(mutation.dependencies) };
    std::map<cost::Coverage, std::size_t> coverage;
    std::vector<OperationEvidence> evidence;
    for (const auto & operation : mutation.operations) {
        const cost::Response * response = nullptr;
        if (const auto * work = std::get_if<std::reference_wrapper<const planning::Work>>(&operation.cost)) {
            if (operation.source_native_work) response = &costs_.query_source_work(work->get(), *operation.source_native_work, operation.resource);
            else if (operation.source_operation) response = &costs_.query_source(work->get(), *operation.source_operation, operation.resource);
            else response = &costs_.query(work->get());
        }
        else if (const auto * delay = std::get_if<planning::SourceRegionGap>(&operation.cost)) response = &costs_.query_region_gap(delay->interval);
        else if (const auto * client = std::get_if<planning::SourceClientDelay>(&operation.cost))
            response = &costs_.query_client_delay(client->request, client->interval);
        else if (const auto * interval = std::get_if<planning::SourceCpuGap>(&operation.cost))
            response = &costs_.query_source_cpu_gap(interval->interval, operation.resource);

        // Copy referenced work only for a missing response or explicit graph
        // diagnostics. Normal prediction keeps the caller's borrowed work.
        const auto describe = [&]() -> CostWork {
            if (const auto * work = std::get_if<std::reference_wrapper<const planning::Work>>(&operation.cost)) return work->get();
            if (const auto * gap = std::get_if<planning::SourceRegionGap>(&operation.cost)) return *gap;
            if (const auto * client = std::get_if<planning::SourceClientDelay>(&operation.cost)) return *client;
            return std::get<planning::SourceCpuGap>(operation.cost);
        };
        if (response && !response->duration_ns) {
            if (seen_gaps_.insert(response).second) gaps_.push_back({ describe(), *response });
            continue;
        }
        if (response) ++coverage[*response->coverage];
        const bool delay = operation.delay || (response && !std::holds_alternative<std::reference_wrapper<const planning::Work>>(operation.cost));
        const auto kind = !response ? graph::NodeKind::Boundary : delay ? graph::NodeKind::Delay : graph::NodeKind::Service;
        bound.operations.push_back({
            { response ? *response->duration_ns : 0, operation.resource, kind },
            operation.completion,
            operation.owner
        });
        if (record_graph_)
            evidence.push_back({ response ? std::optional(describe()) : std::nullopt,
                                 response ? response->coverage : std::nullopt,
                                 operation.owner.value_or(mutation.owner),
                                 operation.completion,
                                 operation.position });
    }
    if (!gaps_.empty()) {
        clock_.stop();
        return std::nullopt;
    }
    if (scaling_.enabled()) {
        for (std::size_t id = 0; id < mutation.operations.size(); ++id) {
            const auto * work = std::get_if<std::reference_wrapper<const planning::Work>>(&mutation.operations[id].cost);
            if (!work) continue;

            const auto & kind = work->get().kind;
            const auto label = kind.operation + ":" + kind.implementation + ":" + kind.timing_scope;
            auto & duration = bound.operations[id].node.duration_ns;
            duration = scaling_.apply(label, duration);
        }
    }
    const auto begin = clock_.submit(std::move(bound));
    operations_.insert(operations_.end(), std::make_move_iterator(evidence.begin()), std::make_move_iterator(evidence.end()));
    for (const auto & [kind, count] : coverage) coverage_[kind] += count;
    return begin;
}

void ProgramExecution::on_event(Execution &, const Event & event) { program_.on_event(*this, event); }

ProgramResult ProgramExecution::run() {
    program_.start(*this);
    if (gaps_.empty()) clock_.run();
    if (gaps_.empty() && !program_.complete()) throw std::runtime_error("No pending event can complete the generated program");

    auto graph = record_graph_ && gaps_.empty() ? std::optional(clock_.generated_graph()) : std::nullopt;
    return {
        clock_.take_result(), std::move(gaps_),      std::move(coverage_), { scaling_.matched().begin(), scaling_.matched().end() },
        std::move(graph),     std::move(operations_)
    };
}

} // namespace markov::trace_graph::simulation
