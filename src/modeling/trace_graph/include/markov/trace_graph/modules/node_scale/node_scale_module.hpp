/**
 * @file
 * @brief NodeScale what-if 模块和结构化执行结果。
 */
#pragma once

#include "markov/trace_graph/frontend/model_config.hpp"
#include "markov/trace_graph/modules/module.hpp"

#include <cstdint>
#include <string>

namespace markov::trace_graph::modules::node_scale {

/**
 * @brief NodeScale 执行后的结构化结果。
 *
 * `scaled_nodes` 是被规则命中的 DAG node 数量，规则配置按原始配置保留，供
 * diagnostics writer 输出 summary。模块本体不负责 JSON 序列化。
 */
struct NodeScaleSummary {
    frontend::NodeScaleConfig config;
    uint64_t scaled_nodes = 0;
};

/**
 * @brief 最简单的 what-if 子模块。
 *
 * NodeScaleModule 按节点 name 的 substring match 找到已有 DAG node，并按 factor 修改 duration。
 * 它不新增/删除节点和边，主要用于验证 SimulationModule 管线和简单 latency 变换。
 */
class NodeScaleModule final : public modules::SimulationModule {
public:
    explicit NodeScaleModule(frontend::NodeScaleConfig config);

    /** @brief 返回 registry / summary 使用的稳定模块名。 */
    [[nodiscard]] std::string name() const override;

    /** @brief 按配置规则修改 DAG node duration。 */
    void apply(core::DagGraph & graph) override;

    /** @brief apply 后提供命中规则的结构化 summary。 */
    [[nodiscard]] bool has_summary() const override;

    /** @brief 读取 NodeScale 执行结果。 */
    [[nodiscard]] NodeScaleSummary summary() const;

private:
    frontend::NodeScaleConfig config_;
    /** @brief apply 后写入 summary，便于验证规则是否实际命中。 */
    uint64_t scaled_nodes_ = 0;
    bool applied_ = false;
};

} // namespace markov::trace_graph::modules::node_scale
