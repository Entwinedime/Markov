/**
 * @file
 * @brief Builds and executes the configured model-module pipeline.
 */
#include "module_pipeline.hpp"

#include "markov/trace_graph/core/logger.hpp"
#include "markov/trace_graph/frontend/model_config.hpp"
#include "markov/trace_graph/modules/hicache/hicache_module.hpp"
#ifdef DEBUG
#include "markov/trace_graph/modules/hicache/dag_patch_module.hpp"
#include "markov/trace_graph/modules/hicache/phase_carrier.hpp"
#endif
#include "markov/trace_graph/modules/node_scale/node_scale_module.hpp"

#include <algorithm>

namespace markov::trace_graph::cli {

#ifdef DEBUG
ModulePipeline ModulePipeline::from_config(const std::string & filename, const std::string & hicache_oracle_cost_replay,
                                           const std::string & hicache_phase_oracle_cost_replay, bool hicache_canonical_observed_phase_scope) {
#else
ModulePipeline ModulePipeline::from_config(const std::string & filename) {
#endif
    ModulePipeline pipeline;
#ifdef DEBUG
    if (hicache_canonical_observed_phase_scope) {
        if (!filename.empty()) throw std::invalid_argument("observed phase carrier is score-only and requires an empty model config");
        pipeline.modules_.push_back(std::make_unique<modules::hicache::HiCacheObservedPhaseCarrierModule>());
        return pipeline;
    }
#endif
    if (filename.empty()) return pipeline;

    const auto config = frontend::ModelConfig::from_file(filename);
    pipeline.modules_.reserve(3);
    if (config.node_scale.enabled) { pipeline.modules_.push_back(std::make_unique<modules::node_scale::NodeScaleModule>(config.node_scale)); }
    if (config.hicache.enabled) {
        bool oracle = false;
#ifdef DEBUG
        oracle = !hicache_oracle_cost_replay.empty() || !hicache_phase_oracle_cost_replay.empty();
#endif
        if (config.hicache.dag_patch_enabled && !oracle) {
            const auto & policy = config.source_prefetch_policy;
            if (policy != "timeout" && policy != "wait_complete" && policy != "best_effort")
                throw std::invalid_argument("HiCache execution requires explicit source_prefetch_policy in the model config");
            pipeline.execution_config_ = config.hicache;
            pipeline.source_prefetch_policy_ = policy;
            return pipeline;
        }
        auto result = std::make_shared<modules::hicache::model::HiCacheModelResult>();
        pipeline.modules_.push_back(std::make_unique<modules::hicache::HiCacheModule>(config.hicache, result));
#ifdef DEBUG
        if (config.hicache.dag_patch_enabled) {
            auto patch = std::make_unique<modules::hicache::HiCacheDagPatchModule>(std::move(result),
                                                                                   config.hicache.dag_patch_source_target_same_config,
                                                                                   hicache_oracle_cost_replay,
                                                                                   hicache_phase_oracle_cost_replay);
            pipeline.modules_.push_back(std::move(patch));
        }
#endif
    }
    return pipeline;
}

bool ModulePipeline::needs_hicache_observations() const {
    return std::ranges::any_of(modules_, [](const auto & module) {
        if (dynamic_cast<const modules::hicache::HiCacheModule *>(module.get())) return true;
#ifdef DEBUG
        if (dynamic_cast<const modules::hicache::HiCacheObservedPhaseCarrierModule *>(module.get())) return true;
#endif
        return false;
    });
}

void ModulePipeline::apply(core::DagGraph & graph, core::Logger & logger, const core::ClientRequestChain & chain, uint64_t begin_us, uint64_t end_us) {
    for (const auto & module : modules_) {
        logger.info() << "Applying module: " << module->name();
        module->apply(graph);
    }
    if (execution_config_) {
        logger.info() << "Executing HiCache HTTP window";
        execution_result_ = modules::hicache::runtime::execute_hicache_window(graph, *execution_config_, source_prefetch_policy_, chain, begin_us, end_us);
    }
}

} // namespace markov::trace_graph::cli
