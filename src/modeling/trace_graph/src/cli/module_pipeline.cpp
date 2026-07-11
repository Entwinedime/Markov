/**
 * @file
 * @brief Builds and executes the configured model-module pipeline.
 */
#include "module_pipeline.hpp"

#include "markov/trace_graph/core/logger.hpp"
#include "markov/trace_graph/frontend/model_config.hpp"
#include "markov/trace_graph/modules/hicache/dag_patch_module.hpp"
#include "markov/trace_graph/modules/hicache/hicache_module.hpp"
#include "markov/trace_graph/modules/node_scale/node_scale_module.hpp"

namespace markov::trace_graph::cli {

ModulePipeline ModulePipeline::from_config(const std::string & filename) {
    ModulePipeline pipeline;
    if (filename.empty()) return pipeline;

    const auto config = frontend::ModelConfig::from_file(filename);
    pipeline.modules_.reserve(3);
    if (config.node_scale.enabled) { pipeline.modules_.push_back(std::make_unique<modules::node_scale::NodeScaleModule>(config.node_scale)); }
    if (config.hicache.enabled) {
        auto result = std::make_shared<modules::hicache::model::HiCacheModelResult>();
        pipeline.modules_.push_back(std::make_unique<modules::hicache::HiCacheModule>(config.hicache, result));
        if (config.hicache.dag_patch_enabled) { pipeline.modules_.push_back(std::make_unique<modules::hicache::HiCacheDagPatchModule>(std::move(result))); }
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
