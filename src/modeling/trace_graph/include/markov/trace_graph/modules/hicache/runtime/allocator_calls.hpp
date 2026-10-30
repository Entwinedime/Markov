#pragma once

#include "markov/trace_graph/adapters/frameworks/sglang/allocator_preparation.hpp"
#include "markov/trace_graph/adapters/frameworks/sglang/allocator_program.hpp"
#include "markov/trace_graph/adapters/frameworks/sglang/hicache_observations.hpp"
#include "markov/trace_graph/modules/hicache/model/replay.hpp"
#include "markov/trace_graph/modules/hicache/runtime/host_program.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_expansion.hpp"
#include "markov/trace_graph/structure/hicache/resources.hpp"

#include <map>
#include <optional>
#include <set>
#include <span>

namespace markov::trace_graph::cost {
class OperatorCostProvider;
}

namespace markov::trace_graph::modules::hicache::runtime {

struct HiCacheSourcePreparation;

/** Replace the inner allocator, not its outer capacity guard or
 * request-index update. State admission supplies target work after eviction;
 * the inner entry executes that work with independently estimated costs. */
class HiCacheAllocatorCalls {
public:
    HiCacheAllocatorCalls(HiCacheWriteStreamInsertions & insertions, cost::OperatorCostProvider & provider, bool compact_programs)
        : insertions_(insertions),
          provider_(provider),
          compact_programs_(compact_programs) {}

    void bind(core::DagGraph & graph, const HiCacheSourcePreparation & prepared, std::span<const model::HiCacheReplayFact> facts,
              const structure::hicache::SchedulerComputeResources & resources, uint64_t begin_us, uint64_t end_us);
    void rebind_resources(const adapters::sglang::HiCacheSourceIndex & source, const std::map<size_t, size_t> & workers);
    void set_work(const model::HiCacheAllocatorWorkItem & work, const adapters::sglang::AllocatorPreparation & preparation);
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
        HostDeviceResources resources;
        adapters::sglang::AscendAllocatorInputs inputs;
        std::optional<model::HiCacheAllocatorWorkItem> source_work;
        HiCacheHostExpansion retained;
        std::map<size_t, HiCacheWriteStreamPosition> positions;
        std::optional<model::HiCacheAllocatorWorkItem> work;
        std::optional<std::string> prefix_dtype;
        std::optional<bool> page_count_supplied;
        std::optional<adapters::sglang::AllocatorPreparation> source_preparation;
        adapters::sglang::AllocatorPreparation preparation;
    };
    HiCacheWriteStreamInsertions & insertions_;
    cost::OperatorCostProvider & provider_;
    bool compact_programs_;
    std::map<size_t, Call> calls_;
    std::map<size_t, size_t> fact_entries_;
    std::set<size_t> owned_;
    size_t executed_ = 0;
    size_t retained_ = 0;
};

} // namespace markov::trace_graph::modules::hicache::runtime
