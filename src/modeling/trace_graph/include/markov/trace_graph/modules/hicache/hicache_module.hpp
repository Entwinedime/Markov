/**
 * @file
 * @brief HiCache state model 接入 SimulationModule 的业务包装层。
 */
#pragma once

#include "markov/trace_graph/modules/hicache/model/state.hpp"
#include "markov/trace_graph/modules/module.hpp"

#include <optional>

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
    HiCacheModule(frontend::HiCacheConfig config, std::optional<frontend::HiCacheConfig> source_config);

    /** @brief 返回 registry / summary 使用的稳定模块名。 */
    [[nodiscard]] std::string name() const override;

    /** @brief 从 DAG 中抽取 HiCache fact，并驱动 canonical state model。 */
    void apply(core::DagGraph & graph) override;

    /** @brief 拓扑仿真后补充依赖 completion_time 的 HiCache critical-path diagnostics。 */
    void after_simulation(core::DagGraph & graph) override;

    /** @brief Debug/validation 构建在 apply 后暴露结构化 summary；Release 不暴露 diagnostics summary。 */
    [[nodiscard]] bool has_summary() const override;

#ifdef DEBUG
    /** @brief 读取最近一次 apply 后生成的 HiCache summary。 */
    [[nodiscard]] const model::HiCacheSummary & summary() const { return summary_; }
#endif

private:
    frontend::HiCacheConfig config_;
    std::optional<frontend::HiCacheConfig> source_config_;
#ifdef DEBUG
    model::HiCacheSummary summary_;
    bool applied_ = false;
#endif
};

} // namespace markov::trace_graph::modules::hicache
