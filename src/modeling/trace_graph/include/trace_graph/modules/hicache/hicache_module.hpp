#pragma once

#include "trace_graph/modules/hicache/hicache_model.hpp"
#include "trace_graph/modules/simulation_module.hpp"

namespace TraceGraph {

// SimulationModule 包装层，负责把 C++ module registry 接入 hicache_model。
// 具体 cache 状态逻辑不要写在这个薄包装类里。
class HiCacheModule final : public SimulationModule {
  public:
    explicit HiCacheModule(HiCacheConfig config);

    std::string name() const override;
    void apply(DagGraph & graph) override;
    bool has_summary() const override;
    std::string summary_json() const override;

    const HiCacheSummary & summary() const { return summary_; }

  private:
    HiCacheConfig config_;
    HiCacheSummary summary_;
    bool applied_ = false;
};

} // namespace TraceGraph
