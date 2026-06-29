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

void HiCacheModule::apply(core::DagGraph & graph) {
    summary_ = model::apply_hicache_model(graph, config_);
    applied_ = true;
}

bool HiCacheModule::has_summary() const { return applied_; }

} // namespace markov::trace_graph::modules::hicache
