/**
 * @file
 * @brief Diagnostic-only static HiCache patch and oracle replay.
 */
#pragma once

#ifdef DEBUG

#include "markov/trace_graph/core/dag_mutation.hpp"
#include "markov/trace_graph/modules/hicache/model/result.hpp"
#include "markov/trace_graph/modules/hicache/patch/applied_validator.hpp"
#include "markov/trace_graph/modules/hicache/patch/attribution.hpp"
#include "markov/trace_graph/modules/hicache/patch/boundary_validator.hpp"
#include "markov/trace_graph/modules/hicache/patch/cpu_collective_waits.hpp"
#include "markov/trace_graph/modules/hicache/patch/io_operation_ledger.hpp"
#include "markov/trace_graph/modules/hicache/patch/io_resource_model.hpp"
#include "markov/trace_graph/modules/hicache/patch/layer_wait_patch.hpp"
#include "markov/trace_graph/modules/hicache/patch/rewrite_transaction.hpp"
#include "markov/trace_graph/modules/hicache/patch/source_dag_index.hpp"
#include "markov/trace_graph/modules/hicache/phase_carrier.hpp"
#include "markov/trace_graph/modules/hicache/runtime/preparation.hpp"
#include "markov/trace_graph/modules/module.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace markov::trace_graph::modules::hicache {

/** Connect request prefetch to reused H2D work in the historical static patch. */
[[nodiscard]] bool append_hicache_reused_loadback_dependencies(const model::HiCachePhaseWorkLedger & phase_work,
                                                               const patch::HiCacheShadowRewriteTransaction & shadow, core::DagMutationPlan & plan);

/** @brief Debug-only exact binding of target-observed costs to predicted phase carriers. */
struct HiCachePhaseOracleCostReplayAudit {
    std::string status = "disabled";
    uint64_t required_cost_count = 0;
    uint64_t supplied_cost_count = 0;
    uint64_t applied_cost_count = 0;
    uint64_t oracle_duration_us = 0;
    uint64_t applied_duration_us = 0;
    bool effect_identity_exact = false;
    bool target_e2e_consumed = false;
};

/** @brief Business result for the HiCache patch plan and applied mutation journal. */
struct HiCacheDagPatchResult {
    std::string status = "not_applied";
    bool source_target_same_config = false;
    std::string phase_patch_status = "disabled";
    size_t phase_duration_update_count = 0;
    size_t phase_owner_conflict_count = 0;
    HiCachePhaseCarrierAudit phase_carrier;
    runtime::AllocatorPreparationPlan runtime_preparation;
    patch::HiCacheLayerWaitPatch layer_wait_patch;
    patch::CpuCollectivePatch cpu_collective_patch;
    core::DagMutationPlan plan;
    core::DagMutationJournal journal;
    patch::HiCacheIoResourcePlan io_resources;
    patch::HiCacheIoOperationLedger io_operation_ledger;
    patch::HiCacheSourceAttributionCatalog source_attribution;
    patch::HiCacheShadowRewriteTransaction shadow_rewrite;
    patch::HiCacheBoundaryValidationCatalog boundary_validation;
    patch::HiCacheAppliedPatchValidation applied_validation;
    std::map<std::string, uint64_t> apply_blockers;
    HiCachePhaseOracleCostReplayAudit phase_oracle_cost_replay;
    core::DagTopologyValidationReport topology;
};

/**
 * @brief Applies the historical static plan for oracle replay and semantic checks.
 *
 * Resource costs and lane dependencies are computed without changing the graph. Concrete
 * rewrites use `DagMutationPlan` and its atomic prevalidation. Ordinary prediction
 * uses runtime::execute_hicache_window, not this module.
 */
class HiCacheDagPatchModule final : public SimulationModule {
public:
    explicit HiCacheDagPatchModule(std::shared_ptr<const model::HiCacheModelResult> model_result, bool source_target_same_config = false);
    HiCacheDagPatchModule(std::shared_ptr<const model::HiCacheModelResult> model_result, bool source_target_same_config, std::string oracle_cost_replay_path,
                          std::string phase_oracle_cost_replay_path);

    [[nodiscard]] std::string_view name() const noexcept override;
    void apply(core::DagGraph & graph) override;
    [[nodiscard]] bool has_summary() const override;
    [[nodiscard]] const HiCacheDagPatchResult & result() const { return result_; }
    [[nodiscard]] const model::HiCacheEffectDecisionLedger & effect_decisions() const { return model_result_->effect_decisions; }

private:
    std::shared_ptr<const model::HiCacheModelResult> model_result_;
    bool source_target_same_config_ = false;
    std::string oracle_cost_replay_path_;
    std::string phase_oracle_cost_replay_path_;
    HiCacheDagPatchResult result_;
    bool applied_ = false;
};

} // namespace markov::trace_graph::modules::hicache

#endif // DEBUG
