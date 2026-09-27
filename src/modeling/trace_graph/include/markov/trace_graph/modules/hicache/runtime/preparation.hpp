/** @file Source-supported allocator preparation demand and CPU gap costs. */
#pragma once

#include "markov/trace_graph/core/dag_mutation.hpp"
#include "markov/trace_graph/modules/hicache/model/phase_work.hpp"
#include <array>
#include <map>
#include <set>

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

struct AllocatorPreparationSourceCall {
    size_t source_fact_id = 0;
    std::string pid, tid;
    uint64_t begin_us = 0, end_us = 0;
    std::optional<AllocatorSpecialization> specialization;
    bool required = false, first_load = false;
    std::string path;
    std::vector<std::pair<uint64_t, uint64_t>> intervals;
    std::optional<size_t> submit_gap_node;
    std::map<std::string, uint64_t> issues;
};

/** Immutable base observations, reusable while the target DAG is executing. */
struct AllocatorPreparationObservation {
    std::vector<AllocatorPreparationSourceCall> calls;
    std::set<std::string> supported_pids;
    bool source_parallel_compilation = false;
    std::map<std::string, std::map<std::string, PreparationCostSamples>> cost_samples;
};

struct AllocatorPreparationCpuCost {
    size_t source_call = 0;
    uint64_t duration_us = 0;
    bool retain_source = false;
};

struct AllocatorPreparationPlan {
    std::string status = "unavailable";
    std::vector<AllocatorPreparation> calls;
    std::vector<AllocatorPreparationCpuCost> cpu_costs;
    std::map<std::string, uint64_t> blockers;
    size_t observed_formal_calls = 0;
    uint64_t removed_coverage_us = 0; // Across ranks; not an E2E saving.
    uint64_t added_cost_us = 0; // Across ranks, not critical-path time.
    bool source_parallel_compilation = false;
    std::map<std::string, std::map<std::string, PreparationCostSamples>> cost_samples;
    core::DagMutationPlan mutation{.component = "runtime_preparation", .reason = "target allocator preparation demand"};
};

[[nodiscard]] AllocatorPreparationObservation observe_allocator_preparations(const core::DagGraph& graph);

/** Predict from base observations and the already-known target call history.
 * No graph mutation or target measurements. The caller must schedule these
 * costs before their work starts; later calls cannot revise executed costs.
 */
[[nodiscard]] AllocatorPreparationPlan predict_allocator_preparations(
    const AllocatorPreparationObservation& source, const std::vector<model::HiCacheAllocatorWorkItem>& calls);

/** Reserve a CPU cost node before each observed allocator submission.
 * Returns source-call index -> node. Measured preparation is moved out of its
 * gap without changing total time; residual time and unrelated edges survive.
 * A call without a submission site is left unbound, never assigned a guessed
 * location. The execution caller must reject any required cost at such a call.
 */
[[nodiscard]] std::map<size_t, size_t> bind_allocator_preparation_costs(
    core::DagGraph& graph, const AllocatorPreparationObservation& source);

[[nodiscard]] AllocatorPreparationPlan plan_allocator_preparations(
    const core::DagGraph& graph, const std::vector<model::HiCacheAllocatorWorkItem>& calls);

} // namespace markov::trace_graph::modules::hicache::runtime
