/**
 * @file
 * @brief HiCache SimulationModule 包装层实现。
 */
#include "markov/trace_graph/modules/hicache/hicache_module.hpp"

#include <string_view>
#include <utility>

namespace markov::trace_graph::modules::hicache {

namespace hicache_module_detail {

constexpr std::string_view kModuleName = "HiCacheModule";

} // namespace hicache_module_detail

using hicache_module_detail::kModuleName;

HiCacheModule::HiCacheModule(frontend::HiCacheConfig config) : config_(std::move(config)) {}

std::string HiCacheModule::name() const { return std::string{ kModuleName }; }

/**
 * @brief 将 HiCache state model 作为 SimulationModule 执行。
 *
 * 该模块当前只产出 summary 和 diagnostics，不直接修改 DAG 节点耗时或边；后续若要做
 * DAG mutation，应在这里保持 module wrapper 与 model core 的职责边界。
 */
void HiCacheModule::apply(core::DagGraph & graph) {
    summary_ = model::apply_hicache_model(graph, config_);
    applied_ = true;
}

bool HiCacheModule::has_summary() const { return applied_; }

} // namespace markov::trace_graph::modules::hicache
