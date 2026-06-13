#pragma once

#include "trace_graph/modules/hicache/hicache_model.hpp"
#include "trace_graph/modules/simulation_module.hpp"

namespace TraceGraph {

/**
 * @brief HiCache 状态模型的 SimulationModule 包装层。
 *
 * 这个类只负责接入 C++ module registry、调用 hicache_model 并保存 summary。
 * 具体 cache 状态逻辑必须留在 hicache_model.cpp，避免 module 包装层变成第二套
 * 状态机入口。
 */
class HiCacheModule final : public SimulationModule {
  public:
    explicit HiCacheModule(HiCacheConfig config);

    std::string name() const override;
    void apply(DagGraph & graph) override;
    bool has_summary() const override;
    std::string summary_json() const override;

    /** @brief 返回最近一次 apply 产生的 HiCache summary。 */
    const HiCacheSummary & summary() const { return summary_; }

  private:
    HiCacheConfig config_;
    HiCacheSummary summary_;
    bool applied_ = false;
};

} // namespace TraceGraph
