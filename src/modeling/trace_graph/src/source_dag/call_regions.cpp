#include "markov/trace_graph/source_dag/call_regions.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace markov::trace_graph::source_dag {

std::set<size_t> interior_stream_placeholders(const SourceDagIndex & source, const std::set<size_t> & owned) {
    const auto & graph = source.graph();
    std::set<size_t> result;
    for (const auto start : owned) {
        if (graph.node(start).is_cpu) continue;

        auto current = start;
        std::vector<size_t> interior;
        while (true) {
            std::optional<size_t> next;
            bool branching = false;
            for (const auto id : source.outgoing_edge_ids(current)) {
                const auto & edge = graph.edge(id);
                if (!edge.active || edge.kind != core::DagEdgeKind::Stream) continue;
                if (next) branching = true;
                next = edge.dst;
            }
            if (branching || !next || !graph.node(*next).active || graph.node(*next).lane_id != graph.node(start).lane_id) break;
            if (owned.contains(*next)) {
                result.insert(interior.begin(), interior.end());
                break;
            }

            const auto & node = graph.node(*next);
            const auto & event = graph.event_for_node(*next);
            if (node.is_cpu || node.duration || event.dur || event.name != "PLACE_HOLDER_SQE") break;

            size_t predecessors = 0;
            bool external = false;
            for (const auto id : source.incoming_edge_ids(*next)) {
                const auto & edge = graph.edge(id);
                if (!edge.active) continue;
                external |= edge.kind != core::DagEdgeKind::Stream || edge.src != current;
                ++predecessors;
            }
            for (const auto id : source.outgoing_edge_ids(*next)) {
                const auto & edge = graph.edge(id);
                external |= edge.active && edge.kind != core::DagEdgeKind::Stream;
            }
            if (external || predecessors != 1) break;

            interior.push_back(*next);
            current = *next;
        }
    }
    return result;
}
std::map<size_t, size_t> match_operator_envelopes(const core::DagGraph & graph, std::span<const size_t> observations,
                                                  const std::map<size_t, std::string> & schemas) {
    std::map<std::string, std::vector<size_t>> expected, observed;
    for (const auto & [position, name] : schemas) expected[name].push_back(position);
    for (const auto index : observations) {
        const auto & event = graph.runtime_observations().at(index);
        if (expected.contains(event.name)) observed[event.name].push_back(index);
    }

    std::map<size_t, size_t> matched;
    for (const auto & [name, operations] : expected) {
        const auto found = observed.find(name);
        // Ordered repeated writes can be paired only when the whole family
        // agrees. Extra nested casts/copies make attribution ambiguous.
        if (found == observed.end() || found->second.size() != operations.size()) continue;
        for (size_t i = 0; i < operations.size(); ++i) matched.emplace(operations[i], found->second[i]);
    }
    return matched;
}

std::optional<DeviceChain> observe_serial_device_chain(const SourceDagIndex & source, const std::set<size_t> & devices) {
    if (devices.empty()) return std::nullopt;
    const auto & graph = source.graph();
    const auto lane = graph.node(*devices.begin()).lane_id;
    DeviceChain result{ *devices.begin(), *devices.begin() };
    size_t links = 0;
    for (const auto node : devices) {
        if (graph.node(node).lane_id != lane) return std::nullopt;
        size_t incoming = 0, outgoing = 0;
        for (const auto edge_id : source.incoming_edge_ids(node)) {
            const auto & edge = graph.edge(edge_id);
            if (!edge.active) continue;
            if (edge.kind == core::DagEdgeKind::Sync) return std::nullopt;
            incoming += edge.kind == core::DagEdgeKind::Stream && devices.contains(edge.src);
        }
        for (const auto edge_id : source.outgoing_edge_ids(node)) {
            const auto & edge = graph.edge(edge_id);
            if (edge.active && edge.kind == core::DagEdgeKind::Stream && devices.contains(edge.dst)) ++outgoing;
        }
        if (incoming > 1 || outgoing > 1) return std::nullopt;
        if (!incoming) result.first = node;
        if (!outgoing) result.last = node;
        links += outgoing;
    }
    // In an acyclic graph, N-1 links and degree at most one prove one chain.
    if (links + 1 != devices.size()) return std::nullopt;
    return result;
}

SubmittedCallRegion observe_submitted_call_region(const SourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues, size_t observation) {
    const auto & graph = source.graph();
    const auto & event = graph.runtime_observations().at(observation);
    SubmittedCallRegion region{ .observation = observation, .caller = source.call_interval_ownership(event.pid, event.tid, event.ts, event.dur) };
    // A partial CPU boundary is not enough to claim downstream work.
    if (region.caller.status != "ready" && region.caller.status != "zero_duration") return region;

    std::set<size_t> owned(region.caller.owned_node_ids.begin(), region.caller.owned_node_ids.end());
    auto pending = region.caller.owned_node_ids;
    for (size_t cursor = 0; cursor < pending.size(); ++cursor) {
        for (const auto edge_id : source.outgoing_edge_ids(pending[cursor])) {
            const auto & edge = graph.edge(edge_id);
            if (!graph.node(edge.dst).active) continue;
            const auto task = queues.node_task.at(edge.src);
            const bool same_task = edge.kind == core::DagEdgeKind::Sequential && task != core::DagNode::kNoNode && queues.node_task.at(edge.dst) == task
                                   && owned.contains(queues.tasks.at(task).submission);
            if (edge.kind != core::DagEdgeKind::Correlation && !same_task) continue;
            if (!owned.insert(edge.dst).second) continue;

            region.submitted_nodes.push_back(edge.dst);
            pending.push_back(edge.dst);
        }
    }
    for (const auto node : pending) {
        for (const auto edge_id : source.incoming_edge_ids(node)) {
            const auto & edge = graph.edge(edge_id);
            if (!owned.contains(edge.src)) region.incoming_edges.push_back(edge_id);
        }
        for (const auto edge_id : source.outgoing_edge_ids(node))
            if (!owned.contains(graph.edge(edge_id).dst)) region.outgoing_edges.push_back(edge_id);
    }
    std::ranges::sort(region.submitted_nodes);
    return region;
}

std::optional<std::vector<QueuedCallTask>> observe_queued_call_tasks(const SourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues,
                                                                     const SubmittedCallRegion & region) {
    if (region.caller.status != "ready" || region.caller.has_node_overlap) return std::nullopt;
    const auto & graph = source.graph();
    const std::set<size_t> caller(region.caller.owned_node_ids.begin(), region.caller.owned_node_ids.end());
    std::map<size_t, QueuedCallTask> tasks;
    std::set<size_t> devices, attributed;
    for (const auto node : region.submitted_nodes) {
        if (!graph.node(node).is_cpu) {
            devices.insert(node);
            continue;
        }
        const auto task = queues.node_task.at(node);
        if (task == core::DagNode::kNoNode || !caller.contains(queues.tasks.at(task).submission)) return std::nullopt;
        tasks.try_emplace(task, QueuedCallTask{ task, {}, {} }).first->second.workers.push_back(node);
    }

    std::vector<QueuedCallTask> result;
    for (auto & [task_id, observed] : tasks) {
        const auto & task = queues.tasks.at(task_id);
        const std::set<size_t> workers(observed.workers.begin(), observed.workers.end());
        if (!workers.contains(task.first) || !workers.contains(task.last)) return std::nullopt;
        for (const auto worker : workers) {
            for (const auto edge_id : source.outgoing_edge_ids(worker)) {
                const auto & edge = graph.edge(edge_id);
                if (edge.kind == core::DagEdgeKind::Sequential && queues.node_task.at(edge.dst) == task_id && !workers.contains(edge.dst)) return std::nullopt;
                if (edge.kind != core::DagEdgeKind::Correlation || graph.node(edge.dst).is_cpu) continue;
                if (!devices.contains(edge.dst) || !attributed.insert(edge.dst).second) return std::nullopt;
                observed.devices.insert(edge.dst);
            }
        }
        std::ranges::sort(observed.workers, {}, [&](size_t node) { return std::pair{ graph.event_for_node(node).ts, node }; });
        result.push_back(std::move(observed));
    }
    if (attributed != devices) return std::nullopt;

    std::ranges::sort(result, {}, [&](const auto & task) {
        const auto submission = queues.tasks.at(task.task).submission;
        return std::pair{ graph.event_for_node(submission).ts, submission };
    });
    return result;
}

} // namespace markov::trace_graph::source_dag
