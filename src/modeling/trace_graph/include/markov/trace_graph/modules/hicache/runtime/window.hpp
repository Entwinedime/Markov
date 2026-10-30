#pragma once

#include "markov/trace_graph/core/client_requests.hpp"
#include "markov/trace_graph/cost/operator_model.hpp"
#include "markov/trace_graph/cost/source_migration.hpp"
#include "markov/trace_graph/modules/hicache/model/result.hpp"
#include "markov/trace_graph/modules/hicache/patch/cpu_collective_waits.hpp"
#include "markov/trace_graph/modules/hicache/runtime/prefetch_waits.hpp"
#include "markov/trace_graph/modules/hicache/runtime/queue_confirmations.hpp"
#include "markov/trace_graph/simulation/topological_simulator.hpp"
#include "markov/trace_graph/structure/hicache/phase.hpp"
#include <nlohmann/json.hpp>

namespace markov::trace_graph::modules::hicache::runtime {

/** Executed-node accounting, not critical-path or HTTP time. Delays remain
 * separate from node duration; worker duration may include modeled residual.
 */
struct ExecutedCostTotals {
    size_t nodes = 0;
    double node_duration_us = 0;
    double sequential_gap_us = 0;
    double ready_delay_us = 0;
};

struct HiCacheWindowResult {
    simulation::SimulationResult simulation;
    model::HiCacheModelResult model;
    patch::CpuCollectivePatch cpu_collectives;
    QueueConfirmationCoverage confirmations;
    cost::SourceCostCoverage phase_operator_costs;
    std::map<std::string, ExecutedCostTotals> executed_cost_sources;
    // Full tensor plans belong to pending calls; reports need only these totals.
    size_t phase_migration_candidates = 0, phase_generated_plans = 0;
    std::map<std::string, size_t> phase_plan_issues;
    // Device completion and reduction worker segments have separate costs.
    std::vector<structure::hicache::DeviceCollectivePlan> collective_work;
    size_t prepared_collective_services = 0, executed_collective_services = 0;
    size_t prepared_collective_events = 0, executed_collective_events = 0;
    size_t prepared_collective_worker_segments = 0, executed_collective_worker_segments = 0;
    size_t prepared_collective_caller_segments = 0, executed_collective_caller_segments = 0;
    size_t prepared_phase_calls = 0, executed_phase_calls = 0;
    size_t prepared_allocator_calls = 0, executed_allocator_calls = 0;
    size_t retained_allocator_calls = 0;
    std::map<std::string, size_t> phase_observation_issues;
    std::map<cost::OperatorCostProvider::QueryKey, cost::OperatorQuerySummary> independent_operator_queries;
    size_t prepared_facts = 0, consumed_facts = 0, phase_admissions = 0;
    size_t decode_allocations = 0;
    size_t admission_retry_rank_calls = 0;
    size_t source_load_submissions = 0, completed_load_batches = 0;
    size_t load_submission_shared_cost_batches = 0;
    size_t prepared_layer_calls = 0, active_layer_calls = 0, inactive_layer_calls = 0;
    size_t source_write_submissions = 0, completed_writes = 0, resumed_allocations = 0;
    size_t observed_write_fragments = 0;
    size_t observed_eviction_fragments = 0;
    size_t expanded_write_submissions = 0;
    size_t generated_control_steps = 0;
    size_t generated_blocking_checks = 0;
    size_t changed_operators = 0;
    uint64_t http_us = 0;
    bool final_cost_transform_enabled = false;
    nlohmann::json performance;
};

/** Execute a serial HTTP window with one live cache state and one DAG clock.
 * Source graph must not have a pre-applied diagnostic scope mask. The caller
 * owns trace loading, source observations and HTTP-chain construction. Tail
 * lifecycle facts remain available; finish never forces background completion.
 * Source policy interprets source branches, target config controls prediction.
 * This reports execution consistency, not complete cost-model coverage/accuracy.
 * An optional final cost transform runs after binding at actual node start;
 * it must be pure and change duration only, never topology or another node's
 * cost. It may also be queried to compare a later binding to executed work.
 */
[[nodiscard]] HiCacheWindowResult execute_hicache_window(core::DagGraph & graph, const core::DagGraph & source_graph, const frontend::HiCacheConfig & config,
                                                         std::string_view source_policy, const core::ClientRequestChain & chain, uint64_t begin_us,
                                                         uint64_t end_us, const simulation::NodeCostAtStart & final_cost_transform = {});

} // namespace markov::trace_graph::modules::hicache::runtime
