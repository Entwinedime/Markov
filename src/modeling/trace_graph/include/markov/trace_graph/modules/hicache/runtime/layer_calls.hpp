#pragma once
#include "markov/trace_graph/cost/operator_model.hpp"
#include "markov/trace_graph/modules/hicache/layer_waits.hpp"
#include "markov/trace_graph/modules/hicache/runtime/host_program.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_expansion.hpp"
#include "markov/trace_graph/source_dag/index.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace markov::trace_graph::modules::hicache::runtime {

class HiCacheLayerCalls {
public:
    explicit HiCacheLayerCalls(HiCacheWriteStreamInsertions & insertions, cost::OperatorCostProvider & provider)
        : insertions_(insertions),
          operator_provider_(provider) {}
    using Consumer = std::function<const std::vector<size_t> &(const std::string &, uint64_t)>;
    void bind(core::DagGraph & graph, uint64_t begin, uint64_t end);
    // Called after all source replacements: queue membership and stream cuts
    // must refer to the same retained graph used by the executor.
    void rebind_retained_resources(const source_dag::SourceDagIndex & source, const std::map<size_t, size_t> & members);
    void advance(size_t node, const Consumer & consumer, simulation::FutureDag & future);
    struct NestedCall {
        size_t anchor;
        std::string pid;
        uint64_t batch_start_ns, layer;
        // Source evidence retained for later cost binding, not branch choice.
        // Split in original coordinates, then corrected with the source CPU
        // service view. The outside portion belongs after the replacement,
        // not to the generated attention operation's service cost.
        uint64_t following_gap_inside_us, following_gap_outside_us;
        std::optional<uint64_t> inactive_return_us;
        HiCacheWriteStreamPosition source_stream_position;
    };
    /** Select whole nested checks in source coordinates without transferring
     * ownership yet. Partial overlap is not safe to replace. Ordering follows
     * source call order, not synthetic node allocation order.
     */
    [[nodiscard]] std::vector<NestedCall> nested_calls(const core::DagGraph & graph, const std::string & pid, const std::string & tid, uint64_t begin_us,
                                                       uint64_t end_us) const;
    /** Remove callbacks only after the corresponding anchors were disabled by
     * the caller's successful atomic replacement. The caller owns their target
     * checks and residual costs thereafter; this method does not edit the DAG.
     */
    void handoff_replaced_calls(const core::DagGraph & graph, std::span<const NestedCall> calls);
    /** Move callbacks onto an unchanged call's copied anchors. Rebind internal
     * successors and stream neighbors using source_nodes, while external cuts
     * use rebind_retained_resources. Target state still selects active/inactive
     * checks; an observed device wait is never migrated as elapsed time. Returns
     * the retained gap outside the enclosing interval for its caller to attach
     * after the replacement; nested callbacks retain only their inside gaps.
     * Invoke after atomic source removal and expansion, before cloned nodes run.
     */
    [[nodiscard]] uint64_t rebind_copied_calls(const core::DagGraph & graph, std::span<const NestedCall> calls, const HiCacheExpandedHost & expanded);
    [[nodiscard]] size_t prepared_calls() const { return calls_.size(); }
    [[nodiscard]] size_t active_calls() const { return active_; }
    [[nodiscard]] size_t inactive_calls() const { return inactive_; }

private:
    void bind_inactive(core::DagGraph & graph, const HiCacheLayerWaitObservation & waits, uint64_t begin, uint64_t end);
    struct Call {
        std::string pid, phase, lane;
        uint64_t batch_start_ns, layer;
        uint64_t source_start_ns, source_end_ns;
        size_t source_position;
        uint64_t outside_gap_us = 0;
        std::optional<core::DagGraph::CpuGapRanges> following_gap_ranges;
        std::optional<uint64_t> source_inactive_us;
        GeneratedLayerWait program;
        HiCacheWriteStreamPosition position;
        size_t device_lane;
        std::vector<size_t> successors, synchronizations;
        std::string insertion_issue;
    };
    void expand_call(size_t entry, const Call & call, const Consumer & consumer, simulation::FutureDag & future);
    std::map<size_t, Call> calls_;
    struct SourceCall {
        size_t anchor;
        uint64_t latest_end_ns;
    };
    // Source-coordinate lookup for phase binding, before execution transfers callbacks.
    std::map<std::pair<std::string, std::string>, std::vector<SourceCall>> source_calls_;
    HiCacheWriteStreamInsertions & insertions_;
    cost::OperatorCostProvider & operator_provider_;
    size_t active_ = 0, inactive_ = 0;
};

} // namespace markov::trace_graph::modules::hicache::runtime
