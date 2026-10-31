#pragma once

#include "markov/trace_graph/adapters/frameworks/sglang/hicache_observations.hpp"
#include "markov/trace_graph/adapters/frameworks/sglang/source_calls.hpp"
#include "markov/trace_graph/modules/hicache/runtime/layer_calls.hpp"
#include "markov/trace_graph/source_dag/call_regions.hpp"
#include "markov/trace_graph/structure/hicache/phase.hpp"
#include "markov/trace_graph/structure/hicache/resources.hpp"

#include <map>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

namespace markov::trace_graph::modules::hicache::runtime {

/** Own per-layer attention, metadata and dense projection regions separately from their target work and costs.
 * Source removal happens before simulation; target work is supplied at request
 * admission and expanded only when its original call boundary executes.
 */
class HiCachePhaseCalls {
public:
    HiCachePhaseCalls(HiCacheLayerCalls & layers, HiCacheWriteStreamInsertions & insertions, const structure::hicache::SchedulerComputeResources & resources,
                      cost::OperatorCostProvider & provider, bool compact_programs, std::vector<std::pair<size_t, size_t>> * retained_source_nodes)
        : layers_(layers),
          insertions_(insertions),
          resources_(resources),
          provider_(provider),
          compact_programs_(compact_programs),
          retained_source_nodes_(retained_source_nodes) {}

    void bind(core::DagGraph & graph, const core::DagGraph & original, std::span<const adapters::sglang::AttentionCall> calls,
              std::span<const adapters::sglang::AttentionMetadataCall> metadata, std::span<const adapters::sglang::DenseCall> dense, uint64_t begin_us,
              uint64_t end_us);
    void rebind_retained_resources(const adapters::sglang::HiCacheSourceIndex & source, const std::map<size_t, size_t> & workers);
    void set_work(structure::hicache::PhaseCallPlan work);
    void advance(core::DagGraph & graph, size_t node, const HiCacheLayerCalls::Consumer & consumer, simulation::FutureDag & future);
    [[nodiscard]] bool owns(size_t source_node) const { return owned_.contains(source_node); }
    [[nodiscard]] size_t prepared() const { return calls_.size() + executed_; }
    [[nodiscard]] size_t executed() const { return executed_; }

private:
    struct Call {
        size_t observation, entry, exit;
        std::string pid, tid;
        uint64_t begin_us;
        HiCacheHostExpansion retained;
        std::vector<HiCacheLayerCalls::NestedCall> layers;
        std::map<size_t, HiCacheWriteStreamPosition> positions;
        std::optional<HostDeviceResources> resources;
    };
    HiCacheLayerCalls & layers_;
    HiCacheWriteStreamInsertions & insertions_;
    const structure::hicache::SchedulerComputeResources & resources_;
    cost::OperatorCostProvider & provider_;
    bool compact_programs_;
    std::vector<std::pair<size_t, size_t>> * retained_source_nodes_;
    std::map<size_t, Call> calls_;
    std::map<size_t, structure::hicache::PhaseCallPlan> work_;
    std::set<size_t> owned_;
    size_t executed_ = 0;
};

} // namespace markov::trace_graph::modules::hicache::runtime
