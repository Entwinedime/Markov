#include "trace_graph/modules/hicache/hicache_model.hpp"

#include "trace_graph/core/dag_graph.hpp"
#include "trace_graph/core/trace_event.hpp"

#include <nlohmann/json.hpp>

namespace TraceGraph {

namespace {

using Json = nlohmann::json;

bool starts_with(const std::string & text, const std::string & prefix) { return text.rfind(prefix, 0) == 0; }

bool contains(const std::string & text, const std::string & needle) { return text.find(needle) != std::string::npos; }

std::string arg_string(const TraceEvent & event, const std::string & key) {
    auto it = event.args.find(key);
    return it == event.args.end() ? "" : it->second;
}

bool is_hicache_event(const TraceEvent & event) {
    // skeleton 阶段只统计输入覆盖，不维护状态。
    // 因此这里的识别条件偏宽，目的是让 reviewer 看到 profiling 是否已经把 HiCache 事实送进 C++ 后端。
    if (event.cat == "hicache") return true;
    if (starts_with(event.name, "HiCache::")) return true;
    if (starts_with(event.name, "hicache_")) return true;
    auto domain = arg_string(event, "domain");
    return domain == "hicache" || (domain == "python_probe" && contains(event.name, "hicache"));
}

} // namespace

std::string HiCacheSummary::to_json() const {
    // summary 只描述 skeleton 的输入覆盖和 warning，不参与 E2E prediction。
    Json root;
    root["status"] = status;
    root["input_hicache_events"] = input_hicache_events;
    root["dag_mutations"] = dag_mutations;
    root["warnings"] = warnings;
    return root.dump();
}

HiCacheSummary apply_hicache_model(DagGraph & graph, const HiCacheConfig & config) {
    HiCacheSummary summary;
    if (!config.enabled) {
        summary.status = "disabled";
        return summary;
    }

    // 当前 HiCache 模块只保留可加载骨架：读取输入事实覆盖情况，但不修改 DAG。
    // 这样可以先验证 base DAG faithful replay，不让旧 cache replay 逻辑干扰后续状态机重建。
    for (const auto & node : graph.nodes()) {
        if (is_hicache_event(graph.event_for_node(node.id))) summary.input_hicache_events++;
    }
    summary.warnings.push_back("HiCacheModule is a skeleton; no DAG mutations are applied.");
    return summary;
}

} // namespace TraceGraph
