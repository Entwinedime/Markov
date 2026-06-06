#pragma once

#include "trace_graph/frontend/model_config.hpp"
#include "trace_graph/modules/simulation_module.hpp"

#include <string>

namespace TraceGraph {

// NodeScaleModule 是最简单的 what-if 子模块：
// 按节点 name 的 substring match 找到已有 DAG node，并按 factor 修改 duration。
// 它不新增/删除节点和边，主要用于验证 SimulationModule 管线和简单 latency 变换。
class NodeScaleModule final : public SimulationModule {
public:
    explicit NodeScaleModule(NodeScaleConfig config);

    std::string name() const override;
    void apply(DagGraph & graph) override;
    bool has_summary() const override;
    std::string summary_json() const override;

private:
    NodeScaleConfig config_;
    // apply 后写入 summary，便于验证规则是否实际命中。
    uint64_t scaled_nodes_ = 0;
    bool applied_ = false;
};

} // namespace TraceGraph
