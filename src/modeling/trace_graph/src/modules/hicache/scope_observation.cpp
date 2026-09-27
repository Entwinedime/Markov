#include "markov/trace_graph/modules/hicache/scope_observation.hpp"
#include "markov/trace_graph/modules/hicache/layer_waits.hpp"
#include "markov/trace_graph/modules/hicache/phase_observation.hpp"
#include <algorithm>

namespace markov::trace_graph::modules::hicache {

HiCacheScopeObservation observe_hicache_scope(const core::DagGraph & graph) {
    HiCacheScopeObservation observed;
    const patch::HiCacheSourceDagIndex source(graph);
    const auto layer_waits = observe_hicache_layer_waits(source);
    const auto phases = observe_hicache_phases(graph, &layer_waits);
    const auto own_nodes = [&](const auto & nodes) { observed.nodes.insert(nodes.begin(), nodes.end()); };
    for (const auto & phase : phases.observations) {
        own_nodes(phase.prefill_common_kernel_node_ids);
        own_nodes(phase.prefill_prefix_attention_node_ids);
        own_nodes(phase.prefill_collective_node_ids);
        own_nodes(phase.prefill_submit_cpu_node_ids);
        own_nodes(phase.decode_kernel_node_ids);
        own_nodes(phase.decode_collective_node_ids);
        own_nodes(phase.decode_submit_cpu_node_ids);
    }
    observed.operations = patch::build_hicache_io_operation_ledger(source);
    const auto own_gaps = [&](const std::vector<patch::HiCacheCpuGapSlice> & slices) {
        for (const auto & slice : slices)
            if (slice.owned_end_us > slice.owned_start_us) observed.gap_intervals[slice.owner_node_id].emplace_back(slice.owned_start_us, slice.owned_end_us);
    };
    own_nodes(layer_waits.cpu_node_ids);
    own_nodes(layer_waits.device_wait_node_ids);
    for (const auto & call : layer_waits.calls)
        if (call.issue.empty()) own_gaps(call.cpu.owned_gap_slices);
    for (const auto & operation : observed.operations.records) {
        own_nodes(operation.runtime_node_ids);
        own_nodes(operation.admission_explicit_node_ids);
        own_nodes(operation.terminal_control_node_ids);
        own_nodes(operation.device_transfer_node_ids);
        own_nodes(operation.device_completion_node_ids);
        own_nodes(operation.readiness_join_node_ids);
        own_nodes(operation.completion_wait_owned_node_ids);
        own_gaps(operation.cpu_gap_slices);
        // Explicit admission/terminal children belong to HiCache. Wrapper/probe
        // self-time and idle submission gaps remain residual, as before.
    }
    for (auto & [node, intervals] : observed.gap_intervals) {
        std::ranges::sort(intervals);
        std::vector<std::pair<uint64_t, uint64_t>> joined;
        for (const auto & interval : intervals) {
            if (joined.empty() || interval.first > joined.back().second) joined.push_back(interval);
            else joined.back().second = std::max(joined.back().second, interval.second);
        }
        intervals = std::move(joined);
    }
    return observed;
}

void apply_observed_hicache_scope(core::DagGraph & graph, const HiCacheScopeObservation & observed) {
    graph.clear_scope_ownership();
    for (const auto id : observed.nodes) graph.set_scope_node_owned(id);
    for (const auto & [node, intervals] : observed.gap_intervals)
        for (const auto & [begin, end] : intervals) graph.add_scope_gap_duration(node, end - begin);
}

patch::HiCacheIoOperationLedger mark_observed_hicache_scope(core::DagGraph & graph) {
    auto observed = observe_hicache_scope(graph);
    apply_observed_hicache_scope(graph, observed);
    return std::move(observed.operations);
}

} // namespace markov::trace_graph::modules::hicache
