#pragma once

#include "trace_graph/frontend/model_config.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace TraceGraph {

class DagGraph;

// HiCache skeleton 的输出摘要。
// 当前只统计输入 HiCache 事件覆盖，不维护 page 状态，也不修改 DAG。
struct HiCacheSummary {
    std::string status = "skeleton";
    uint64_t input_hicache_events = 0;
    uint64_t dag_mutations = 0;
    std::vector<std::string> warnings;

    std::string to_json() const;
};

// HiCache 功能模型入口。
// 后续 cache 状态机、policy、DAG mutation 都应从这个函数继续拆分实现。
HiCacheSummary apply_hicache_model(DagGraph & graph, const HiCacheConfig & config);

} // namespace TraceGraph
