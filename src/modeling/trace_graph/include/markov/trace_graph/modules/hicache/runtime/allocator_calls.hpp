#pragma once

#include "markov/trace_graph/modules/hicache/model/replay.hpp"
#include "markov/trace_graph/modules/hicache/runtime/load_execution.hpp"
#include "markov/trace_graph/modules/hicache/runtime/preparation.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_expansion.hpp"
#include "markov/trace_graph/structure/hicache/allocator.hpp"
#include "markov/trace_graph/structure/hicache/resources.hpp"

#include <map>
#include <optional>
#include <set>
#include <span>

namespace markov::trace_graph::cost {
class OperatorCostProvider;
}

namespace markov::trace_graph::modules::hicache::runtime {

/** Replace the inner allocator, not its outer capacity guard or
 * request-index update. State admission supplies target work after eviction;
 * the inner entry executes that work with independently estimated costs. */
class HiCacheAllocatorCalls {
public:
    HiCacheAllocatorCalls(HiCacheWriteStreamInsertions & insertions, cost::OperatorCostProvider & provider) : insertions_(insertions), provider_(provider) {}

    void bind(core::DagGraph & graph, const core::DagGraph & original, std::span<const model::HiCacheReplayFact> facts,
              const structure::hicache::SchedulerComputeResources & resources, const AllocatorPreparationObservation & preparation, uint64_t begin_us,
              uint64_t end_us);
    void rebind_resources(const patch::HiCacheSourceDagIndex & source, const std::map<size_t, size_t> & workers);
    void set_work(const model::HiCacheAllocatorWorkItem & work, const structure::hicache::AllocatorPreparation & preparation);
    void advance(core::DagGraph & graph, size_t node, simulation::FutureDag & future);
    [[nodiscard]] bool owns(size_t source_node) const { return owned_.contains(source_node); }
    [[nodiscard]] size_t prepared() const { return calls_.size(); }
    [[nodiscard]] size_t executed() const { return executed_; }
    [[nodiscard]] size_t retained() const { return retained_; }

private:
    struct Call {
        size_t exit;
        std::string pid, tid;
        uint64_t begin_us;
        LoadIndexResources resources;
        structure::hicache::AscendAllocatorInputs inputs;
        std::optional<model::HiCacheAllocatorWorkItem> source_work;
        HiCacheHostExpansion retained;
        std::map<size_t, HiCacheWriteStreamPosition> positions;
        std::optional<model::HiCacheAllocatorWorkItem> work;
        std::optional<std::string> prefix_dtype;
        std::optional<bool> page_count_supplied;
        std::optional<structure::hicache::AllocatorPreparation> source_preparation;
        structure::hicache::AllocatorPreparation preparation;
    };
    HiCacheWriteStreamInsertions & insertions_;
    cost::OperatorCostProvider & provider_;
    std::map<size_t, Call> calls_;
    std::map<size_t, size_t> fact_entries_;
    std::set<size_t> owned_;
    size_t executed_ = 0;
    size_t retained_ = 0;
};

} // namespace markov::trace_graph::modules::hicache::runtime
