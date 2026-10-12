/** @file Allocator preparation observations, static costs and graph binding. */
#pragma once

#include "markov/trace_graph/core/dag_mutation.hpp"
#include "markov/trace_graph/modules/hicache/model/phase_work.hpp"
#include "markov/trace_graph/structure/hicache/allocator.hpp"
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace markov::trace_graph::modules::hicache::runtime {

struct PreparationCostSamples {
    size_t count = 0;
    uint64_t minimum_us = 0, median_us = 0, maximum_us = 0;
};

struct AllocatorPreparationSourceCall {
    size_t source_fact_id = 0;
    std::string pid, tid;
    uint64_t begin_us = 0, end_us = 0;
    std::optional<structure::hicache::AllocatorSpecialization> specialization;
    bool required = false, first_load = false;
    std::string path;
    std::vector<std::pair<uint64_t, uint64_t>> intervals;
    uint64_t service_duration_us = 0; // Same-source CPU correction over the interval union.
    std::optional<size_t> submit_gap_node;
    std::map<std::string, uint64_t> issues;
    // Exact inner-call metadata for whole allocator replacement. These are
    // source observations, independent of preparation timing measurements.
    std::optional<size_t> allocator_observation;
    std::optional<model::HiCacheAllocatorWorkItem> allocator_work;
    std::optional<std::string> prefix_dtype;
    std::optional<bool> page_count_supplied;
    std::vector<model::HiCacheAllocatorRequest> requests;
};

/** Immutable base observations, reusable while the target DAG is executing. */
struct AllocatorPreparationObservation {
    std::vector<AllocatorPreparationSourceCall> calls;
    std::set<std::string> supported_pids;
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
    std::vector<structure::hicache::AllocatorPreparation> calls;
    std::vector<AllocatorPreparationCpuCost> cpu_costs;
    std::map<std::string, uint64_t> blockers;
    size_t observed_formal_calls = 0;
    uint64_t removed_coverage_us = 0; // Across ranks; not an E2E saving.
    uint64_t added_cost_us = 0;       // Across ranks, not critical-path time.
    std::optional<bool> source_parallel_compilation;
    std::map<std::string, std::map<std::string, PreparationCostSamples>> cost_samples;
    core::DagMutationPlan mutation{ .component = "runtime_preparation", .reason = "target allocator preparation demand" };
};

[[nodiscard]] AllocatorPreparationObservation observe_allocator_preparations(const core::DagGraph & graph);

/** Historical static diagnostic, including source sample medians. Formal
 * prediction reads raw observations and uses its independent cost provider. */
[[nodiscard]] AllocatorPreparationPlan plan_allocator_preparations(const core::DagGraph & graph, const std::vector<model::HiCacheAllocatorWorkItem> & calls);

} // namespace markov::trace_graph::modules::hicache::runtime
