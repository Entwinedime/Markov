#include "markov/trace_graph/adapters/frameworks/sglang/hicache_observations.hpp"
#include "markov/trace_graph/modules/hicache/execution_boundaries.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_calls.hpp"
#include "markov/trace_graph/structure/missing_work.hpp"
#include <algorithm>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::runtime {

void HiCacheWriteCalls::bind_lifecycle_writes(core::DagGraph & graph, uint64_t begin_us) {
    std::map<size_t, std::vector<uint64_t>> source_submissions;
    for (const auto & [node, actions] : actions_)
        for (const auto & [action, index] : actions)
            if (action == Action::Submit) source_submissions[calls_[index].owner].push_back(calls_[index].at_us);

    std::vector<core::TraceEvent> points;
    std::vector<size_t> owners;
    for (const auto & insert : lifecycle_inserts_) {
        const auto append = [&](core::TraceEvent point) {
            // Match the lifecycle state entry for work crossing the opening
            // cut. Keep its raw envelope and readiness proof unchanged.
            if (point.ts < begin_us) {
                point.ts = begin_us;
                point.ts_submicro_ns = 0;
            }
            point.dur = point.dur_submicro_ns = 0;
            points.push_back(std::move(point));
            owners.push_back(insert.owner);
        };
        const auto steps = lifecycle_ready_steps(insert);
        for (size_t i = 0; i < steps.size(); ++i) {
            const auto until_us = i + 1 < steps.size() ? steps[i + 1].ready.ts : insert.envelope.ts + insert.envelope.dur;
            const auto observed = source_submissions.find(insert.owner);
            // Keep an observed submission at its own host call, but only for
            // this path step. A different target page size can add writes at
            // other steps or at insertion return, even for the same owner.
            if (observed != source_submissions.end()
                && std::ranges::any_of(observed->second, [&](uint64_t at_us) { return at_us >= steps[i].ready.ts && at_us < until_us; }))
                continue;

            append(steps[i].ready);
        }

        auto end = insert.envelope;
        const auto ns = end.ts_submicro_ns + end.dur_submicro_ns;
        end.ts += end.dur + ns / 1'000;
        end.ts_submicro_ns = ns % 1'000;
        append(std::move(end));
    }
    const auto nodes = bind_hicache_control_points(graph, points);
    const adapters::sglang::HiCacheSourceIndex source(graph);
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
            const auto resources = write_resources_.find({ call.pid, call.tid });
            if (resources == write_resources_.end())
                throw structure::MissingWork("Lifecycle write has no observed scheduler resources",
                                             {
                                                 {               "pid",                       call.pid },
                                                 {               "tid",                       call.tid },
                                                 { "required_resource", "scheduler_compute_and_worker" }
                });
            const auto expanded = submit_target_write(resources->second, fact, pending, sequence, future);
            for (const auto successor : call.successors) future.depend(expanded.host_return, successor);
        }
    }
}

} // namespace markov::trace_graph::modules::hicache::runtime
