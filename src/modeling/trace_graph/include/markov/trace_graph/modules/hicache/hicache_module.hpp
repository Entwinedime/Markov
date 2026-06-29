/**
 * @file
 * @brief HiCache state model 接入 SimulationModule 的业务包装层。
 */
#pragma once

#include "markov/trace_graph/modules/hicache/model/state.hpp"
#include "markov/trace_graph/modules/module.hpp"

namespace markov::trace_graph::modules::hicache {

/**
 * @brief HiCache 状态模型的 SimulationModule 包装层。
 *
 * 包装层只负责接入 module registry 和暴露 summary，具体状态逻辑集中在
 * `HiCacheState` 状态对象。
 */
class HiCacheModule final : public SimulationModule {
public:
    explicit HiCacheModule(frontend::HiCacheConfig config);

    /** @brief 返回 registry / summary 使用的稳定模块名。 */
    [[nodiscard]] std::string name() const override;

    /** @brief 从 DAG 中抽取 HiCache fact，并驱动 canonical state model。 */
    void apply(core::DagGraph & graph) override;

    /** @brief HiCache 模块总是提供结构化 summary。 */
    [[nodiscard]] bool has_summary() const override;

    /** @brief 读取最近一次 apply 后生成的 HiCache summary。 */
    [[nodiscard]] const model::HiCacheSummary & summary() const { return summary_; }

private:
    frontend::HiCacheConfig config_;
    model::HiCacheSummary summary_;
    bool applied_ = false;
};

} // namespace markov::trace_graph::modules::hicache
