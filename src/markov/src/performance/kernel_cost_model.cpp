#include "markov/performance/kernel_cost_model.hpp"

#include "markov/contracts/json_input.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>

namespace markov {
KernelCostModel::KernelCostModel(const nlohmann::json & entries) {
    for (const auto & entry : entries) {
        const auto features = KernelCostFeatures::read(entry.at("features"));
        const auto duration = input::integer(entry.at("estimate_ns"), 0, std::numeric_limits<std::int64_t>::max());
        auto & launches = entries_[{ features.tensor_descriptor(), features.attention, features.operands }];
        if (std::ranges::any_of(launches,
                                [&](const Launch & launch) { return launch.task_type == features.task_type && launch.blocks == features.launch_blocks; }))
            throw std::invalid_argument("duplicate kernel cost features");
        launches.push_back({ features.task_type, features.launch_blocks, duration, features.tensor_traversal });
    }
}

std::optional<std::int64_t> KernelCostModel::measured_cost(const KernelCostFeatures & features) const {
    const auto found = entries_.find({ features.tensor_descriptor(), features.attention, features.operands });
    if (found == entries_.end()) return std::nullopt;
    for (const auto & launch : found->second)
        if (launch.task_type == features.task_type && launch.blocks == features.launch_blocks && launch.tensor_traversal == features.tensor_traversal)
            return launch.duration_ns;
    return std::nullopt;
}

CostModelResult KernelCostModel::estimate(const CostQuery & query) const {
    if (query.kind != CostQueryKind::Kernel || query.issue != CostQueryIssue::None) return std::unexpected(CostModelGap{});
    const auto * features = &std::get<KernelCostQuery>(query.features);

    auto found = entries_.find({ features->tensors, features->attention, features->operands });
    if (found == entries_.end() && !features->operands.empty()) {
        // Only an existing producer-declared tensor traversal may omit the
        // value operands. A physical-only non-traversal sample cannot match.
        found = entries_.find({ features->tensors, features->attention, {} });
        if (found != entries_.end() && std::ranges::none_of(found->second, &Launch::tensor_traversal)) found = entries_.end();
    }
    if (found == entries_.end()) return std::unexpected(CostModelGap{});
    if (found->second.size() != 1) return std::unexpected(CostModelGap{ CostQueryIssue::FeaturesUndefined, "kernel_launch_mapping_undefined" });

    // The producer's traversal assumption can explicitly project semantic
    // values out of this key. Other unmodeled loops/indexing remain gaps.
    const auto & launch = found->second.front();
    if (!launch.tensor_traversal && !features->unmodeled_operands.empty())
        return std::unexpected(CostModelGap{ CostQueryIssue::FeaturesUndefined, "kernel_operand_projection_undefined" });
    return launch.duration_ns;
}

} // namespace markov
