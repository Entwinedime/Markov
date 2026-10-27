#pragma once

#include "markov/trace_graph/adapters/frameworks/sglang/allocator_preparation.hpp"
#include "markov/trace_graph/core/dag_mutation.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace markov::trace_graph::modules::hicache::diagnostics {

struct PreparationCostSamples {
    size_t count = 0;
    uint64_t minimum_us = 0, median_us = 0, maximum_us = 0;
};

struct AllocatorPreparationCpuCost {
    size_t source_call = 0;
    uint64_t duration_us = 0;
    bool retain_source = false;
};

/** Static replay combines work, observed aggregate costs and graph patching.
 * Formal execution uses the cost-free work plan and a separate cost result.
 */
struct AllocatorPreparationPlan {
    std::string status = "unavailable";
    std::vector<adapters::sglang::AllocatorPreparation> calls;
    std::vector<AllocatorPreparationCpuCost> cpu_costs;
    std::map<std::string, uint64_t> blockers;
    size_t observed_formal_calls = 0;
    uint64_t removed_coverage_us = 0; // Across ranks; not an E2E saving.
    uint64_t added_cost_us = 0;       // Across ranks, not critical-path time.
    std::optional<bool> source_parallel_compilation;
    std::map<std::string, std::map<std::string, PreparationCostSamples>> cost_samples;
    core::DagMutationPlan mutation{ .component = "runtime_preparation", .reason = "target allocator preparation demand" };
};

/** Historical static diagnostic using source medians, not formal prediction. */
[[nodiscard]] AllocatorPreparationPlan plan_allocator_preparations(const core::DagGraph & graph, const std::vector<model::HiCacheAllocatorWorkItem> & calls);

} // namespace markov::trace_graph::modules::hicache::diagnostics
