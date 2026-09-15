/**
 * @file
 * @brief Source-independent semantic carriers for Prefill/Decode work.
 */
#pragma once

#include "markov/trace_graph/core/dag_mutation.hpp"
#include "markov/trace_graph/modules/hicache/model/phase_work.hpp"
#ifdef DEBUG
#include "markov/trace_graph/modules/hicache/phase_observation.hpp"
#include "markov/trace_graph/modules/module.hpp"
#endif

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace markov::trace_graph::modules::hicache {

/** Stable semantic identity used when Direct and phase work share a request boundary. */
[[nodiscard]] std::string hicache_phase_carrier_synthetic_id(std::string_view request_id,
                                                              int logical_input,
                                                              std::string_view phase,
                                                              std::string_view family);

/** @brief Completeness and mutation counts for one canonical phase-carrier plan. */
struct HiCachePhaseCarrierAudit {
    std::string status = "not_built";
    size_t request_rank_count = 0;
    size_t request_count = 0;
    size_t synthetic_carrier_count = 0;
    size_t source_device_node_count = 0;
    size_t source_submit_node_count = 0;
    size_t dependency_count = 0;
    size_t owner_conflict_count = 0;
    std::map<std::string, uint64_t> blockers;
};

/**
 * @brief Appends one request/rank semantic phase graph to an existing atomic plan.
 *
 * Source device nodes retain their dependency identity at zero cost. Predicted work is
 * carried by synthetic common/prefix/collective and decode nodes whose topology depends
 * on request semantics, not on whether the source happened to expose a target I/O carrier.
 */
[[nodiscard]] HiCachePhaseCarrierAudit append_hicache_phase_carrier_plan(const core::DagGraph & graph,
                                                                          const model::HiCachePhaseWorkLedger & phase_work,
                                                                          core::DagMutationPlan & plan);

#ifdef DEBUG
/** @brief Debug score-side result for canonicalizing target-observed phase work. */
struct HiCacheObservedPhaseCarrierResult {
    std::string status = "not_applied";
    HiCachePhaseObservationAudit observed;
    struct DirectCarrierAudit {
        std::string status = "not_built";
        size_t observed_operation_count = 0;
        size_t canonical_prefetch_count = 0;
        size_t synthetic_node_count = 0;
        size_t dependency_count = 0;
        std::map<std::string, uint64_t> blockers;
    } direct;
    HiCachePhaseCarrierAudit carrier;
    core::DagMutationJournal journal;
    core::DagTopologyValidationReport topology;
};

/**
 * @brief Converts target-observed phase costs to the same semantic carrier graph used by prediction.
 *
 * This module is score-only. It has no model parameters and may only be enabled on a
 * target observation run without a model config.
 */
class HiCacheObservedPhaseCarrierModule final : public SimulationModule {
public:
    [[nodiscard]] std::string_view name() const noexcept override;
    void apply(core::DagGraph & graph) override;
    [[nodiscard]] bool has_summary() const override { return applied_; }
    [[nodiscard]] const HiCacheObservedPhaseCarrierResult & result() const { return result_; }

private:
    HiCacheObservedPhaseCarrierResult result_;
    bool applied_ = false;
};
#endif

} // namespace markov::trace_graph::modules::hicache
