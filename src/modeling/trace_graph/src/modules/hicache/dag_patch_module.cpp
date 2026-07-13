/**
 * @file
 * @brief Read-only HiCache source attribution and resource planning.
 */
#include "markov/trace_graph/modules/hicache/dag_patch_module.hpp"

#include "markov/trace_graph/core/numeric.hpp"

#ifdef DEBUG
#include <chrono>
#endif
#include <stdexcept>
#include <utility>

namespace markov::trace_graph::modules::hicache {

HiCacheDagPatchModule::HiCacheDagPatchModule(std::shared_ptr<const model::HiCacheModelResult> model_result) : model_result_(std::move(model_result)) {
    if (!model_result_) throw std::invalid_argument("HiCacheDagPatchModule requires a shared model result");
}

std::string_view HiCacheDagPatchModule::name() const noexcept { return "HiCacheDagPatchModule"; }

namespace {

enum class PatchPhase : std::uint8_t {
    IoResource,
    SourceIndex,
    SourceAttribution,
    RewritePlanning,
    BoundaryValidation,
    MutationApply,
    AppliedValidation,
};

class PatchProfiler {
public:
    explicit PatchProfiler(HiCacheDagPatchResult & result)
#ifdef DEBUG
        : timings_(result.timings),
          started_at_(std::chrono::steady_clock::now())
#endif
    {
#ifndef DEBUG
        (void)result;
#endif
    }

    template <typename Function> auto measure(PatchPhase phase, Function && function) {
#ifdef DEBUG
        const auto start = std::chrono::steady_clock::now();
        auto value = std::forward<Function>(function)();
        record(phase, elapsed_ms(start));
        return value;
#else
        (void)phase;
        return std::forward<Function>(function)();
#endif
    }

    void finish() {
#ifdef DEBUG
        timings_.total_ms = elapsed_ms(started_at_);
#endif
    }

private:
#ifdef DEBUG
    static uint64_t elapsed_ms(std::chrono::steady_clock::time_point start) {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
    }

    void record(PatchPhase phase, uint64_t elapsed) {
        switch (phase) {
        case PatchPhase::IoResource:
            timings_.io_resource_ms = elapsed;
            break;
        case PatchPhase::SourceIndex:
            timings_.source_index_ms = elapsed;
            break;
        case PatchPhase::SourceAttribution:
            timings_.source_attribution_ms = elapsed;
            break;
        case PatchPhase::RewritePlanning:
            timings_.rewrite_planning_ms = elapsed;
            break;
        case PatchPhase::BoundaryValidation:
            timings_.boundary_validation_ms = elapsed;
            break;
        case PatchPhase::MutationApply:
            timings_.mutation_apply_ms = elapsed;
            break;
        case PatchPhase::AppliedValidation:
            timings_.applied_validation_ms = elapsed;
            break;
        }
    }

    HiCacheDagPatchTimings & timings_;
    std::chrono::steady_clock::time_point started_at_;
#endif
};

void add_apply_blocker(HiCacheDagPatchResult & result, std::string blocker) {
    (void)core::checked_increment_u64(result.apply_blockers[std::move(blocker)], "HiCache patch apply-blocker count exceeds uint64 range");
}

void build_apply_gate(HiCacheDagPatchResult & result, const model::HiCacheModelResult & model_result) {
    if (model_result.effect_decisions.status != "ready") add_apply_blocker(result, "target_decision_ledger_not_ready");
    if (result.io_resources.status != "ready") add_apply_blocker(result, "io_resource_plan_not_ready");
    if (!result.shadow_rewrite.plan.empty() && !result.io_resources.calibrated_for_apply()) add_apply_blocker(result, "io_model_not_calibrated");
    if (result.source_attribution.status != "ready") add_apply_blocker(result, "source_attribution_not_ready");
    if (result.shadow_rewrite.status != "ready") add_apply_blocker(result, "shadow_rewrite_not_ready");
    if (!result.shadow_rewrite.topology_valid) add_apply_blocker(result, "shadow_topology_invalid");
    if (result.boundary_validation.status != "ready") add_apply_blocker(result, "boundary_validation_not_ready");
}

core::DagMutationPlan blocked_plan() {
    return core::DagMutationPlan{
        .plan_id = "hicache_patch_blocked",
        .reason = "production apply gates are not satisfied",
    };
}

core::DagMutationPlan executable_plan(const patch::HiCacheShadowRewriteTransaction & shadow) {
    auto plan = shadow.plan;
    plan.plan_id = "hicache_dag_patch_transaction";
    plan.reason = "complete target-derived HiCache direct-effect transaction";
    return plan;
}

} // namespace

void HiCacheDagPatchModule::apply(core::DagGraph & graph) {
    if (!model_result_->replay_complete) throw std::logic_error("HiCacheDagPatchModule must run after HiCacheModule");

    PatchProfiler profiler(result_);
    result_.prefill_effect_status = model_result_->effect_decisions.prefill_effect_status;
    result_.prefetch_readiness_status = model_result_->effect_decisions.prefetch_readiness_status;
    result_.io_resources = profiler.measure(PatchPhase::IoResource, [&] { return patch::build_hicache_io_resource_plan(model_result_->effect_decisions); });
    {
        auto source_index = profiler.measure(PatchPhase::SourceIndex, [&] { return patch::HiCacheSourceDagIndex(graph); });
        result_.source_index = source_index.stats();
        result_.source_attribution = profiler.measure(PatchPhase::SourceAttribution,
                                                      [&] { return patch::build_hicache_source_attribution(source_index, model_result_->effect_decisions); });
    }
    result_.shadow_rewrite = profiler.measure(PatchPhase::RewritePlanning, [&] {
        return patch::build_hicache_shadow_rewrite_transaction(graph, model_result_->effect_decisions, result_.source_attribution, result_.io_resources);
    });
    result_.boundary_validation =
        profiler.measure(PatchPhase::BoundaryValidation, [&] { return patch::validate_hicache_shadow_boundaries(graph, result_.shadow_rewrite); });
    build_apply_gate(result_, *model_result_);
    result_.plan = result_.apply_blockers.empty() ? executable_plan(result_.shadow_rewrite) : blocked_plan();
    auto mutation = profiler.measure(PatchPhase::MutationApply, [&] { return core::apply_dag_mutation_plan(graph, result_.plan); });
    if (result_.apply_blockers.empty()) {
#ifdef DEBUG
        const bool materialized_topology_valid = mutation.topology.ok();
#else
        constexpr bool materialized_topology_valid = true;
#endif
        result_.applied_validation = profiler.measure(PatchPhase::AppliedValidation, [&] {
            return patch::validate_hicache_applied_patch(graph, result_.shadow_rewrite, result_.io_resources, mutation.journal, materialized_topology_valid);
        });
        if (result_.applied_validation.status != "ready") throw std::logic_error("materialized HiCache DAG patch failed post-apply semantic validation");
    }
    result_.journal = std::move(mutation.journal);
#ifdef DEBUG
    result_.topology = std::move(mutation.topology);
#endif
    if (!result_.apply_blockers.empty()) result_.status = "blocked";
    else result_.status = result_.plan.empty() ? "no_mutation_required" : "applied";
    profiler.finish();
#ifdef DEBUG
    applied_ = true;
#endif
}

#ifdef DEBUG
bool HiCacheDagPatchModule::has_summary() const { return applied_; }
#endif

} // namespace markov::trace_graph::modules::hicache
