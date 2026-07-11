/**
 * @file
 * @brief Phase 0/1 execution of the empty HiCache DAG mutation plan.
 */
#include "markov/trace_graph/modules/hicache/dag_patch_module.hpp"

#include <stdexcept>
#include <utility>

namespace markov::trace_graph::modules::hicache {

HiCacheDagPatchModule::HiCacheDagPatchModule(std::shared_ptr<const model::HiCacheModelResult> model_result) : model_result_(std::move(model_result)) {
    if (!model_result_) throw std::invalid_argument("HiCacheDagPatchModule requires a shared model result");
}

std::string_view HiCacheDagPatchModule::name() const noexcept { return "HiCacheDagPatchModule"; }

void HiCacheDagPatchModule::apply(core::DagGraph & graph) {
    if (!model_result_->replay_complete) throw std::logic_error("HiCacheDagPatchModule must run after HiCacheModule");

    result_.plan = core::DagMutationPlan{
        .plan_id = "hicache_phase01_empty",
        .effect_id = {},
        .reason = "effect attribution and concrete DAG patch are deferred until Phase 3+",
    };
    auto mutation = core::apply_dag_mutation_plan(graph, result_.plan);
    result_.journal = std::move(mutation.journal);
#ifdef DEBUG
    result_.topology = std::move(mutation.topology);
#endif
    result_.status = "empty_plan_applied";
#ifdef DEBUG
    applied_ = true;
#endif
}

#ifdef DEBUG
bool HiCacheDagPatchModule::has_summary() const { return applied_; }
#endif

} // namespace markov::trace_graph::modules::hicache
