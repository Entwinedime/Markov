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

// HiCache 当前只保留 skeleton 开关。真实状态机和 DAG mutation 后续在该配置下扩展。
struct HiCacheConfig {
    bool enabled = false;
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
