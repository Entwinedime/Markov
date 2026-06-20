#pragma once

#include "trace_graph/modules/hicache/hicache_model.hpp"
#include "trace_graph/modules/simulation_module.hpp"

namespace TraceGraph {

/**
 * @brief HiCache 状态模型的 SimulationModule 包装层。
 *
 * 包装层只负责接入 module registry 和暴露 summary，具体状态逻辑集中在
 * HiCacheState。
 */
class HiCacheModule final : public SimulationModule {
public:
    explicit HiCacheModule(HiCacheConfig config);

    [[nodiscard]] std::string name() const override;
    void apply(DagGraph & graph) override;
    [[nodiscard]] bool has_summary() const override;
    [[nodiscard]] std::string summary_json() const override;
    [[nodiscard]] const HiCacheSummary & summary() const { return summary_; }

private:
    HiCacheConfig config_;
    HiCacheSummary summary_;
    bool applied_ = false;
};

} // namespace TraceGraph
