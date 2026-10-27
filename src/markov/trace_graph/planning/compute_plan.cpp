#include "markov/trace_graph/planning/compute_plan.hpp"

#include <compare>
#include <map>
#include <optional>
#include <utility>

namespace markov::trace_graph::planning {

ComputePlan project_compute(const source::SourceContext & source, source::Index frame, const AttentionInput & variables) {
    struct Binding {
        source::Index description;
        std::optional<RowBinding> rows;
        auto operator<=>(const Binding &) const = default;
    };
    const auto & observed = source.observations();
    const auto phase = observed.compute_frames[frame].phase;
    ComputePlan result;
    std::map<Binding, std::size_t> bindings;
    for (const auto operation : source.frame_operations(frame)) {
        const auto & call = observed.work_observations[operation];
        const auto & definition = observed.work_descriptions[call.description].operation;
        const bool attention = std::holds_alternative<AttentionGeometry>(definition.geometry);
        if (!attention && !call.rows) {
            result.missing_row_bindings.push_back(operation);
            continue;
        }
        const Binding key{ call.description, call.rows };
        const auto [binding, inserted] = bindings.try_emplace(key, result.work.size());
        if (inserted) {
            ComputeInput input = variables;
            if (!attention) {
                const auto items = call.rows->basis == RowBasis::QueryTokens ? variables.query_tokens : variables.batch_size;
                input = RowInput{ items * call.rows->rows_per_item };
            }
            result.work.push_back(definition.describe(phase, input));
        }
        const auto unchanged = result.work[binding->second] == observed.work_descriptions[call.description].observed;
        result.operations.push_back({ binding->second, operation, unchanged });
    }
    return result;
}

} // namespace markov::trace_graph::planning
