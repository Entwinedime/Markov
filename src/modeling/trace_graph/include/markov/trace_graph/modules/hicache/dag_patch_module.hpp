/**
 * @file
 * @brief HiCache effect resource planning and DAG mutation orchestration.
 */
#pragma once

#include "markov/trace_graph/core/dag_mutation.hpp"
#include "markov/trace_graph/modules/hicache/model/result.hpp"
#include "markov/trace_graph/modules/hicache/patch/applied_validator.hpp"
#include "markov/trace_graph/modules/hicache/patch/attribution.hpp"
#include "markov/trace_graph/modules/hicache/patch/boundary_validator.hpp"
#include "markov/trace_graph/modules/hicache/patch/io_resource_model.hpp"
#include "markov/trace_graph/modules/hicache/patch/rewrite_transaction.hpp"
#include "markov/trace_graph/modules/hicache/patch/source_dag_index.hpp"
#include "markov/trace_graph/modules/module.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace markov::trace_graph::modules::hicache {

#ifdef DEBUG
/** @brief Debug-only wall-clock breakdown of one HiCache DAG patch application. */
struct HiCacheDagPatchTimings {
    uint64_t io_resource_ms = 0;
    uint64_t source_index_ms = 0;
    uint64_t source_attribution_ms = 0;
    uint64_t rewrite_planning_ms = 0;
    uint64_t boundary_validation_ms = 0;
    uint64_t mutation_apply_ms = 0;
    uint64_t applied_validation_ms = 0;
    uint64_t total_ms = 0;
};
#endif

/** @brief Business result for the HiCache patch plan and applied mutation journal. */
struct HiCacheDagPatchResult {
    std::string status = "not_applied";
    std::string prefill_effect_status = "deferred";
    std::string prefetch_readiness_status = "payload_only_control_pipeline_unmodeled";
    core::DagMutationPlan plan;
    core::DagMutationJournal journal;
    patch::HiCacheIoResourcePlan io_resources;
    patch::HiCacheSourceDagIndexStats source_index;
    patch::HiCacheSourceAttributionCatalog source_attribution;
    patch::HiCacheShadowRewriteTransaction shadow_rewrite;
    patch::HiCacheBoundaryValidationCatalog boundary_validation;
    patch::HiCacheAppliedPatchValidation applied_validation;
    std::map<std::string, uint64_t> apply_blockers;
#ifdef DEBUG
    HiCacheDagPatchTimings timings;
    core::DagTopologyValidationReport topology;
#endif
};

/**
 * @brief Consumes an immutable state-replay result and applies one validated DAG plan.
 *
 * Resource costs and lane dependencies are computed without changing the graph. Concrete
 * rewrites remain empty until source attribution is available, and future mutations must use
 * `DagMutationPlan` so they cannot bypass atomic prevalidation.
 */
class HiCacheDagPatchModule final : public SimulationModule {
public:
    explicit HiCacheDagPatchModule(std::shared_ptr<const model::HiCacheModelResult> model_result);

    [[nodiscard]] std::string_view name() const noexcept override;
    void apply(core::DagGraph & graph) override;
#ifdef DEBUG
    [[nodiscard]] bool has_summary() const override;
#endif
    [[nodiscard]] const HiCacheDagPatchResult & result() const { return result_; }

private:
    std::shared_ptr<const model::HiCacheModelResult> model_result_;
    HiCacheDagPatchResult result_;
#ifdef DEBUG
    bool applied_ = false;
#endif
};

} // namespace markov::trace_graph::modules::hicache
