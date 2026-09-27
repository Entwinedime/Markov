#pragma once
#include "markov/trace_graph/core/client_requests.hpp"
#include "markov/trace_graph/modules/hicache/model/result.hpp"
#include "markov/trace_graph/modules/hicache/patch/cpu_collective_waits.hpp"
#include "markov/trace_graph/modules/hicache/runtime/queue_confirmations.hpp"
#include "markov/trace_graph/modules/hicache/runtime/prefetch_waits.hpp"
#include "markov/trace_graph/simulation/topological_simulator.hpp"

namespace markov::trace_graph::modules::hicache::runtime {

struct HiCacheWindowResult {
    simulation::SimulationResult simulation;
    model::HiCacheModelResult model;
    patch::CpuCollectivePatch cpu_collectives;
    QueueConfirmationCoverage confirmations;
    size_t prepared_facts = 0, consumed_facts = 0, phase_admissions = 0;
    size_t decode_allocations = 0;
    size_t source_load_submissions = 0, completed_load_batches = 0;
    size_t prepared_layer_calls = 0, active_layer_calls = 0, inactive_layer_calls = 0;
    size_t source_write_submissions = 0, completed_writes = 0, resumed_allocations = 0;
    size_t prepared_write_templates = 0;
    size_t prepared_eviction_controls = 0;
    size_t expanded_write_submissions = 0;
    size_t calibrated_empty_checks = 0;
    size_t generated_control_steps = 0;
    size_t generated_blocking_checks = 0, blocking_check_cost_extrapolations = 0;
    size_t phase_extrapolated_control_steps = 0;
    uint64_t calibrated_empty_check_cpu_us = 0;
    size_t preparation_slots = 0, executed_preparations = 0, changed_operators = 0;
    uint64_t http_us = 0;
    std::string wait_template_request, wait_calibration_manifest;
    std::string query_calibration_manifest;
    std::map<int, std::string> local_return_calibration_manifests;
};

/** Execute a serial HTTP window with one live cache state and one DAG clock.
 * Source graph must not have a pre-applied diagnostic scope mask. The caller
 * owns trace loading, source observations and HTTP-chain construction. Tail
 * lifecycle facts remain available; finish never forces background completion.
 * Source policy interprets source branches, target config controls prediction.
 * This reports execution consistency, not complete cost-model coverage/accuracy.
 */
[[nodiscard]] HiCacheWindowResult execute_hicache_window(core::DagGraph & graph, const frontend::HiCacheConfig & config, std::string_view source_policy,
                                                         const core::ClientRequestChain & chain, uint64_t begin_us, uint64_t end_us,
                                                         PrefetchWaitObserver wait_observer = {});

} // namespace markov::trace_graph::modules::hicache::runtime
