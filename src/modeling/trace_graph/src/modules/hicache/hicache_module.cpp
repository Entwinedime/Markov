#include "trace_graph/modules/hicache/hicache_module.hpp"

#include <utility>

namespace TraceGraph {

HiCacheModule::HiCacheModule(HiCacheConfig config) : config_(std::move(config)) {}

std::string HiCacheModule::name() const { return "HiCacheModule"; }

void HiCacheModule::apply(DagGraph & graph) {
    // 功能模块入口保持极薄：真正状态机逻辑放在 hicache_model.cpp，
    // 以后 debug 类也应绑定模块 summary/state，而不是在这里直接写文件。
    summary_ = apply_hicache_model(graph, config_);
    applied_ = true;
}

bool HiCacheModule::has_summary() const { return applied_; }

std::string HiCacheModule::summary_json() const { return "{\"name\":\"HiCacheModule\",\"hicache\":" + summary_.to_json() + "}"; }

} // namespace TraceGraph
