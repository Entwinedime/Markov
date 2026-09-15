/**
 * @file
 * @brief Builds and executes the configured model-module pipeline.
 */
#include "module_pipeline.hpp"

#include "markov/trace_graph/core/logger.hpp"
#include "markov/trace_graph/frontend/model_config.hpp"
#include "markov/trace_graph/modules/hicache/dag_patch_module.hpp"
#include "markov/trace_graph/modules/hicache/hicache_module.hpp"
#ifdef DEBUG
#include "markov/trace_graph/modules/hicache/phase_carrier.hpp"
#endif
#include "markov/trace_graph/modules/node_scale/node_scale_module.hpp"

namespace markov::trace_graph::cli {

#ifdef DEBUG
ModulePipeline ModulePipeline::from_config(const std::string & filename,
                                           const std::string & hicache_oracle_cost_replay,
                                           const std::string & hicache_phase_oracle_cost_replay,
                                           bool hicache_canonical_observed_phase_scope) {
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
        auto result = std::make_shared<modules::hicache::model::HiCacheModelResult>();
        pipeline.modules_.push_back(std::make_unique<modules::hicache::HiCacheModule>(config.hicache, result));
        if (config.hicache.dag_patch_enabled) {
#ifdef DEBUG
            auto patch = std::make_unique<modules::hicache::HiCacheDagPatchModule>(std::move(result),
                                                                                   config.hicache.dag_patch_source_target_same_config,
                                                                                   hicache_oracle_cost_replay,
                                                                                   hicache_phase_oracle_cost_replay);
#else
            auto patch = std::make_unique<modules::hicache::HiCacheDagPatchModule>(std::move(result), config.hicache.dag_patch_source_target_same_config);
#endif
            pipeline.modules_.push_back(std::move(patch));
        }
    }
    return pipeline;
}

void ModulePipeline::apply(core::DagGraph & graph, core::Logger & logger) const {
    for (const auto & module : modules_) {
        logger.info() << "Applying module: " << module->name();
        module->apply(graph);
    }
}

} // namespace markov::trace_graph::cli
