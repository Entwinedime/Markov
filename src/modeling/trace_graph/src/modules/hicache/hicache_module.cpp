/**
 * @file
 * @brief HiCache SimulationModule wrapper。
 */
#include "trace_graph/modules/hicache/hicache_module.hpp"

#include <string_view>
#include <utility>

namespace TraceGraph {

namespace {

constexpr std::string_view kModuleName = "HiCacheModule";

} // namespace

HiCacheModule::HiCacheModule(HiCacheConfig config) : config_(std::move(config)) {}

std::string HiCacheModule::name() const { return std::string{ kModuleName }; }

void HiCacheModule::apply(DagGraph & graph) {
    summary_ = apply_hicache_model(graph, config_);
    applied_ = true;
}

bool HiCacheModule::has_summary() const { return applied_; }

std::string HiCacheModule::summary_json() const { return "{\"name\":\"" + std::string{ kModuleName } + "\",\"hicache\":" + summary_.to_json() + "}"; }

} // namespace TraceGraph
