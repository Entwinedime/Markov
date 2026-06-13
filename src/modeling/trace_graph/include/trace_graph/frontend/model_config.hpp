#pragma once

#include <string>
#include <vector>

namespace TraceGraph {

// 简单节点缩放子模块的单条规则。
// name 当前是 substring match，不是正则；factor 只作用于匹配节点的 duration。
struct NodeScaleRuleConfig {
    std::string id;
    std::string name;
    double factor = 1.0;
};

struct NodeScaleConfig {
    bool enabled = false;
    std::vector<NodeScaleRuleConfig> rules;
};

// HiCache 当前先维护 cache state；DAG mutation 后续在该配置下继续扩展。
struct HiCacheConfig {
    bool enabled = false;
    uint64_t page_size = 0;
    uint64_t l1_capacity_pages = 0;
    uint64_t l2_capacity_pages = 0;
    std::string write_policy = "write_through";
    uint64_t write_through_threshold = 0;
    std::string prefetch_policy = "timeout";
    uint64_t prefetch_threshold_pages = 0;
    uint64_t prefetch_capacity_limit_pages = 0;
    bool prefetch_timeout_configured = false;
    double prefetch_timeout_base_sec = 0.0;
    double prefetch_timeout_per_ki_token_sec = 0.0;
    double prefetch_timeout_max_sec = 0.0;
    bool emit_state_digests = false;
};

// C++ 后端消费的模型配置。
// Python model_runner 会把上层 modeling config 转换成这份更窄的 C++ model config。
struct ModelConfig {
    std::vector<std::string> modules;
    NodeScaleConfig node_scale;
    HiCacheConfig hicache;

    // 判断 modules 列表中是否显式启用了某个模块。
    bool module_enabled(const std::string & name) const;
    static ModelConfig from_file(const std::string & filename);
};

} // namespace TraceGraph
