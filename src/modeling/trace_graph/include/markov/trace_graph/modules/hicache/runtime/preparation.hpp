/** @file Source-supported allocator preparation demand and CPU gap costs. */
#pragma once

#include "markov/trace_graph/core/dag_mutation.hpp"
#include "markov/trace_graph/modules/hicache/model/phase_work.hpp"
#include <array>
#include <map>

namespace markov::trace_graph::modules::hicache::runtime {

// page size, batch upper bound, extend-token upper bound, free pointer mod 16.
using AllocatorSpecialization = std::array<uint64_t, 4>;

struct AllocatorPreparation {
    std::string status;
    std::optional<AllocatorSpecialization> specialization;
    std::string path = "unknown";
    bool first_load = false;
};

struct PreparationCostSamples {
    size_t count = 0;
    uint64_t minimum_us = 0, median_us = 0, maximum_us = 0;
};

struct AllocatorPreparationPlan {
    std::string status = "unavailable";
    std::vector<AllocatorPreparation> calls;
    std::map<std::string, uint64_t> blockers;
    size_t observed_formal_calls = 0;
    uint64_t removed_coverage_us = 0; // Across ranks; not an E2E saving.
    uint64_t added_cost_us = 0; // Across ranks, not critical-path time.
    bool source_parallel_compilation = false;
    std::map<std::string, std::map<std::string, PreparationCostSamples>> cost_samples;
    core::DagMutationPlan mutation{.component = "runtime_preparation", .reason = "target allocator preparation demand"};
};

[[nodiscard]] AllocatorPreparationPlan plan_allocator_preparations(
    const core::DagGraph& graph, const std::vector<model::HiCacheAllocatorWorkItem>& calls);

} // namespace markov::trace_graph::modules::hicache::runtime
