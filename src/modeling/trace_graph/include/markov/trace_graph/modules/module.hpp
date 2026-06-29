/**
 * @file
 * @brief trace_graph what-if 模块的业务执行接口。
 */
#pragma once

#include "markov/trace_graph/core/dag_graph.hpp"

#include <string>

namespace markov::trace_graph::modules {

/**
 * @brief 所有 what-if 的 C++ 模块接口。
 *
 * 模块可以读取 graph 中的 trace fact、维护内部状态、修改 node/edge/duration，
 * 但不应该修改原始 trace 文件，也不应该把 debug 信息混入默认 prediction.json。
 */
class SimulationModule {
public:
    virtual ~SimulationModule() = default;

    /** @brief 稳定模块名，用于 registry、summary 和 debug 归属。 */
    [[nodiscard]] virtual std::string name() const = 0;

    /** @brief 唯一执行入口；模块之间的顺序由 CLI 根据 model config 固定。 */
    virtual void apply(core::DagGraph & graph) = 0;

    /** @brief summary 是显式打开的辅助输出；默认预测路径不依赖它。 */
    [[nodiscard]] virtual bool has_summary() const { return false; }
};

} // namespace markov::trace_graph::modules
