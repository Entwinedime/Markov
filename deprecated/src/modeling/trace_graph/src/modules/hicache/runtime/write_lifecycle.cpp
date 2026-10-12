#include "markov/trace_graph/modules/hicache/execution_boundaries.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_calls.hpp"
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::runtime {

void HiCacheWriteCalls::bind_lifecycle_writes(core::DagGraph & graph) {
    std::vector<core::TraceEvent> points;
    std::vector<size_t> owners;
    for (const auto & insert : lifecycle_inserts_) {
        // Existing source submissions are consumed at their own call sites.
        if (source_owners_.contains(insert.owner)) continue;
        const auto append = [&](core::TraceEvent point) {
            point.dur = point.dur_submicro_ns = 0;
            points.push_back(std::move(point));
            owners.push_back(insert.owner);
        };
        for (const auto & step : lifecycle_ready_steps(insert)) append(step.ready);
        auto end = insert.envelope;
        const auto ns = end.ts_submicro_ns + end.dur_submicro_ns;
        end.ts += end.dur + ns / 1'000;
        end.ts_submicro_ns = ns % 1'000;
        append(std::move(end));
    }
    const auto nodes = bind_hicache_control_points(graph, points);
    const patch::HiCacheSourceDagIndex source(graph);
    for (size_t i = 0; i < points.size(); ++i) {
        if (!nodes[i]) throw std::runtime_error("Lifecycle write lacks an exact ready boundary");
        Call call{ .owner = owners[i], .pid = points[i].pid, .tid = points[i].tid, .at_us = points[i].ts, .entry_node = *nodes[i] };
        for (const auto edge_id : source.outgoing_edge_ids(*nodes[i])) {
            const auto & edge = graph.edge(edge_id);
            if (edge.active && graph.node(edge.dst).active) call.successors.push_back(edge.dst);
        }
        if (const auto resources = write_resources_.find({ call.pid, call.tid }); resources != write_resources_.end()) {
            for (const auto node : { resources->second.compute, resources->second.transfer }) {
                const auto lane = graph.node(node).lane_id;
                if (!call.positions.contains(lane)) call.positions.emplace(lane, observe_write_stream_position(source, node, call.pid, call.tid, call.at_us));
            }
        }
        lifecycle_write_owners_.insert(call.owner);
        lifecycle_writes_at_[*nodes[i]].push_back(std::move(call));
    }
}

void HiCacheWriteCalls::submit_lifecycle_writes(size_t node, uint64_t time, simulation::FutureDag & future) {
    const auto at = lifecycle_writes_at_.find(node);
    if (at == lifecycle_writes_at_.end()) return;
    for (const auto & call : at->second) {
        auto fact = replay_.fact(call.owner).fact;
        fact.ts = time;
        fact.execution_anchor_node_id = node;
        HiCacheHostSequence sequence(call.positions, stream_insertions_, future);
        for (const auto & pending : replay_.state().pending_device_writes(fact)) {
            const auto key = std::pair{ pending.header.cache_scope, pending.header.operation_id };
            if (pending.header.source_node_id != call.owner || completions_.contains(key)) continue;
            const auto & donor = write_resources_.at({ call.pid, call.tid });
            const auto expanded = submit_target_write(donor, fact, pending, sequence, future);
            for (const auto successor : call.successors) future.depend(expanded.host_return, successor);
        }
    }
}

} // namespace markov::trace_graph::modules::hicache::runtime
