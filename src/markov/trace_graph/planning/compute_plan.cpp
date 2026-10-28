#include "markov/trace_graph/planning/compute_plan.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <utility>

namespace markov::trace_graph::planning {

ComputePlan project_compute(const source::SourceContext & source, source::Index frame, const AttentionInput & variables) {
    const auto & observed = source.observations();
    const auto phase = observed.compute_frames[frame].phase;
    ComputePlan result;
    std::map<source::Index, std::vector<std::size_t>> bindings;
    std::map<source::Index, std::pair<std::size_t, std::size_t>> ports;
    for (const auto operation : source.frame_operations(frame)) {
        const auto & call = observed.work_observations[operation];
        const auto & description = observed.work_descriptions[call.description];
        if (std::ranges::any_of(description.recipe, [](const ComputePart & part) { return !part.operation.uses_frame_input() && !part.rows; })) {
            result.missing_row_bindings.push_back(operation);
            continue;
        }
        const auto [binding, inserted] = bindings.try_emplace(call.description);
        if (inserted) {
            for (const auto & part : description.recipe) {
                ComputeInput input = variables;
                if (!part.operation.uses_frame_input()) {
                    const auto items = part.rows->basis == RowBasis::QueryTokens ? variables.query_tokens : variables.batch_size;
                    input = RowInput{ items * part.rows->rows_per_item };
                }
                for (auto & work : part.operation.describe(phase, input)) {
                    binding->second.push_back(result.work.size());
                    result.work.push_back(std::move(work));
                }
            }
        }
        const auto & original = description.observed;
        const auto unchanged =
            std::ranges::equal(original, binding->second, {}, std::identity{}, [&](std::size_t id) -> const Work & { return result.work[id]; });
        const auto begin = result.operations.size();
        if (binding->second.empty()) result.operations.push_back({ std::nullopt, operation, unchanged });
        for (const auto work : binding->second) {
            const auto id = result.operations.size();
            result.operations.push_back({ work, operation, unchanged });
            if (id != begin) result.dependencies.push_back({ id - 1, id, graph::EdgeKind::DataDependency });
        }
        ports.emplace(operation, std::pair(begin, result.operations.size() - 1));
    }
    for (const auto & order : source.frame_queue_order(frame)) {
        const auto before = ports.find(order.before), after = ports.find(order.after);
        if (before != ports.end() && after != ports.end())
            result.queue_dependencies.push_back({ before->second.second, after->second.first, graph::EdgeKind::ResourceOrder });
    }
    std::ranges::sort(result.queue_dependencies, {}, &graph::Edge::from);
    return result;
}

} // namespace markov::trace_graph::planning
