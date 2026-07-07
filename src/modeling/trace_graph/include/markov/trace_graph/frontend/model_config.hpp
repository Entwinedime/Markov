/**
 * @file
 * @brief Python runner 传给 C++ trace_graph 后端的窄配置模型。
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace markov::trace_graph::frontend {

/**
 * @brief 简单节点缩放子模块的单条规则。
 *
 * name 当前是 substring match，不是正则；factor 只作用于匹配节点的 duration。
 */
struct NodeScaleRuleConfig {
    std::string id;
    std::string name;
    double factor = 1.0;
};

/** @brief NodeScaleModule 的窄配置，仅描述模块启用状态和缩放规则。 */
struct NodeScaleConfig {
    bool enabled = false;
    std::vector<NodeScaleRuleConfig> rules;
};

/**
 * @brief HiCache state 模块消费的显式 target config。
 *
 * HiCache 当前先维护 cache state；DAG mutation 后续在该配置下继续扩展。
 * 这里承载 target 策略和容量事实，不读取 observed/default policy。
 */
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
    bool device_allocator_need_sort = false;
    bool emit_state_digests = false;
    bool enable_dag_patch = false;
};

/**
 * @brief C++ 后端消费的模型配置。
 *
 * Python model_runner 会把上层 modeling config 转换成这份更窄的 C++ model config。
 */
struct ModelConfig {
    std::vector<std::string> modules;
    NodeScaleConfig node_scale;
    HiCacheConfig hicache;

    /** @brief 判断 modules 列表中是否显式启用了某个模块。 */
    [[nodiscard]] bool module_enabled(const std::string & name) const;

    /** @brief 从窄 C++ model config JSON 文件加载配置。 */
    [[nodiscard]] static ModelConfig from_file(const std::string & filename);
};

} // namespace markov::trace_graph::frontend
