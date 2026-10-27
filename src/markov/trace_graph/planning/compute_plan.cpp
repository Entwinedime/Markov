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
    for (const auto operation : source.frame_operations(frame)) {
        const auto & call = observed.work_observations[operation];
        const auto & description = observed.work_descriptions[call.description];
        if (std::ranges::any_of(description.recipe,
                                [](const ComputePart & part) { return !std::holds_alternative<AttentionGeometry>(part.operation.geometry) && !part.rows; })) {
            result.missing_row_bindings.push_back(operation);
            continue;
        }
        const auto [binding, inserted] = bindings.try_emplace(call.description);
        if (inserted) {
            for (const auto & part : description.recipe) {
                ComputeInput input = variables;
                if (!std::holds_alternative<AttentionGeometry>(part.operation.geometry)) {
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
        for (const auto work : binding->second) {
            const auto id = result.operations.size();
            result.operations.push_back({ work, operation, unchanged });
            if (id != begin) result.dependencies.push_back({ id - 1, id, graph::EdgeKind::DataDependency });
        }
    }
    return result;
}

} // namespace markov::trace_graph::planning
