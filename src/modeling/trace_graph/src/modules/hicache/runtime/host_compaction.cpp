#include "modules/hicache/runtime/host_compaction.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_expansion.hpp"

#include <numeric>
#include <utility>
#include <vector>

namespace markov::trace_graph::modules::hicache::runtime {

void compact_host_program(HiCacheHostExpansion & plan, std::span<const size_t> source_callbacks) {
    const auto count = plan.nodes.size(), absent = core::DagNode::kNoNode;
    std::vector<size_t> incoming(count), outgoing(count), previous(count, absent), next(count, absent), parent(count), compact_ids(count, absent);
    std::vector<bool> ports(count, false), sequential(count, false), continues_task(count, false);
    std::iota(parent.begin(), parent.end(), size_t{ 0 });
    ports.at(plan.host_return) = true;
    for (const auto & edge : plan.edges) {
        ++incoming.at(edge.to);
        ++outgoing.at(edge.from);
        previous[edge.to] = edge.from;
        next[edge.from] = edge.to;
        sequential[edge.from] = edge.kind == core::DagEdgeKind::Sequential;
    }
    for (const auto & node : plan.nodes) {
        if (node.submission) ports.at(*node.submission) = true;
        if (node.task_predecessor) continues_task.at(*node.task_predecessor) = true;
    }
    for (size_t id = 0; id < count; ++id)
        if (plan.nodes[id].observe_start || (plan.nodes[id].submission && !continues_task[id])) ports[id] = true;
    for (const auto & stream : plan.streams) {
        ports.at(stream.first) = true;
        ports.at(stream.last) = true;
    }
    for (const auto * waits : { &plan.waits, &plan.event_waits, &plan.collective_completions })
        for (const auto & wait : *waits) ports.at(wait.consumer) = true;
    for (const auto source : source_callbacks) ports.at(plan.source_nodes.at(source)) = true;

    // Residual gaps may move only between private completions, never across a launch or task release.
    const auto can_merge = [&](size_t first, size_t second) {
        if (first == absent || second == absent || ports[first] || ports[second] || outgoing[first] != 1 || incoming[second] != 1 || !sequential[first]
            || outgoing[second] != 1 || !sequential[second])
            return false;
        const auto & before = plan.nodes[first];
        const auto & after = plan.nodes[second];
        return before.work.is_cpu && after.work.is_cpu && before.work.lane_key == after.work.lane_key && !before.work.counts_toward_e2e
               && !after.work.counts_toward_e2e && before.submission == after.submission && before.queue_member == after.queue_member
               && !after.work.cpu_task_ready_delay_us;
    };
    bool changed = false;
    for (size_t head = 0; head < count; ++head) {
        if (can_merge(previous[head], head)) continue;
        auto current = head;
        while (can_merge(current, next[current])) {
            const auto tail = next[current];
            auto & aggregate = plan.nodes[head].work;
            const auto & member = plan.nodes[tail].work;
            aggregate.duration = core::checked_add_u64(aggregate.duration, member.duration, "Retained CPU service overflow");
            aggregate.cpu_gap_after = core::checked_add_u64(aggregate.cpu_gap_after, member.cpu_gap_after, "Retained CPU residual overflow");
            aggregate.name = "retained serial CPU work";
            parent[tail] = head;
            current = tail;
            changed = true;
        }
    }
    if (!changed) return;

    size_t retained_nodes = 0;
    for (size_t id = 0; id < count; ++id) retained_nodes += parent[id] == id;
    std::vector<HiCacheHostExpansion::Node> nodes;
    nodes.reserve(retained_nodes);
    for (size_t id = 0; id < count; ++id) {
        if (parent[id] != id) continue;
        compact_ids[id] = nodes.size();
        nodes.push_back(std::move(plan.nodes[id]));
    }
    for (size_t id = 0; id < count; ++id) compact_ids[id] = compact_ids[parent[id]];
    // Queue witnesses and wait sources keep their global source identities.
    for (auto & node : nodes) {
        if (node.submission) *node.submission = compact_ids[*node.submission];
        if (node.task_predecessor) *node.task_predecessor = compact_ids[*node.task_predecessor];
    }
    std::vector<HiCacheHostExpansion::Edge> edges;
    edges.reserve(plan.edges.size() - (count - retained_nodes));
    for (auto edge : plan.edges) {
        const bool self_loop = edge.from == edge.to;
        edge.from = compact_ids[edge.from];
        edge.to = compact_ids[edge.to];
        if (self_loop || edge.from != edge.to) edges.push_back(edge);
    }
    plan.edges = std::move(edges);
    for (auto & stream : plan.streams) {
        stream.first = compact_ids[stream.first];
        stream.last = compact_ids[stream.last];
    }
    for (auto * waits : { &plan.waits, &plan.event_waits, &plan.collective_completions })
        for (auto & wait : *waits) wait.consumer = compact_ids[wait.consumer];
    for (auto & [source, local] : plan.source_nodes) local = compact_ids[local];
    plan.host_return = compact_ids[plan.host_return];
    plan.nodes = std::move(nodes);
}

} // namespace markov::trace_graph::modules::hicache::runtime
