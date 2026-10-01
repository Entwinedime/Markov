#include "markov/trace_graph/modules/hicache/runtime/host_removal.hpp"
#include "markov/trace_graph/core/dag_builder.hpp"
#include "markov/trace_graph/simulation/cpu_task_queues.hpp"
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace markov::trace_graph::modules::hicache::runtime {

core::DagNodeRef append_host_entry_gate(const patch::HiCacheSourceDagIndex & source, size_t first, core::DagObservedPoint point, std::string id,
                                        std::string name, core::DagMutationPlan & plan) {
    const auto & graph = source.graph();
    const auto gate = core::DagNodeRef::synthetic(std::move(id));
    plan.synthetic_nodes.push_back({
        gate.synthetic_id,
        { .name = std::move(name), .category = "execution_gate", .lane_key = std::string(graph.node_lane_key(first)), .observed_point = std::move(point) }
    });
    for (const auto edge : source.incoming_edge_ids(first))
        if (graph.edge(edge).active && graph.edge(edge).dst == first) plan.redirect_edges.push_back({ .edge_index = edge, .dst = gate });

    plan.add_edges.push_back({ gate, core::DagNodeRef::existing(first), core::DagEdgeKind::Sequential });
    return gate;
}

core::DagMutationPlan plan_host_removal(const patch::HiCacheSourceDagIndex & source, std::span<const HiCacheHostRegion> regions) {
    using Kind = core::DagEdgeKind;
    const auto & graph = source.graph();
    core::DagMutationPlan plan{ .component = "hicache_host_replacement" };
    std::set<size_t> removed, workers, gates;
    std::map<size_t, std::pair<size_t, size_t>> main;
    std::set<std::tuple<size_t, size_t, Kind>> additions;
    for (const auto & region : regions) {
        const auto & host = *region.host;
        const auto & first = graph.event_for_node(region.entry);
        const auto & last = graph.event_for_node(region.exit);
        if (region.entry == region.exit || !graph.node(region.entry).active || !graph.node(region.exit).active || !graph.node(region.entry).is_cpu
            || !graph.node(region.exit).is_cpu || first.ts != host.main.interval_start_us || last.ts != host.main.interval_end_us
            || graph.node(region.entry).duration || graph.node(region.exit).duration || graph.node(region.entry).lane_id != graph.node(region.exit).lane_id
            || host.main.status != "ready" || host.main.has_node_overlap)
            throw std::runtime_error("Host removal requires exact empty entry/exit gates and complete ownership");
        gates.insert(region.entry);
        gates.insert(region.exit);
        std::set<size_t> path;
        auto current = region.entry;
        while (current != region.exit) {
            std::vector<size_t> next;
            for (const auto id : source.outgoing_edge_ids(current)) {
                const auto & edge = graph.edge(id);
                if (edge.active && edge.kind == Kind::Sequential && graph.node(edge.dst).lane_id == graph.node(region.entry).lane_id) next.push_back(edge.dst);
            }
            if (next.size() != 1) throw std::runtime_error("Host removal has no unique CPU entry-to-exit path");
            current = next.front();
            if (current == region.exit) break;
            if (!path.insert(current).second || !main.emplace(current, std::pair{ region.entry, region.exit }).second)
                throw std::runtime_error("Host removal CPU regions overlap or contain a cycle");
        }
        const std::set<size_t> owned(host.main.owned_node_ids.begin(), host.main.owned_node_ids.end());
        for (const auto id : owned)
            if (!path.contains(id)) throw std::runtime_error("Host work lies outside its entry-to-exit path");
        for (const auto id : path) {
            const auto & event = graph.event_for_node(id);
            if (!owned.contains(id) && (graph.node(id).duration || event.dur || event.ts < first.ts || event.ts > last.ts))
                throw std::runtime_error("Host path contains unowned executable work");
        }
        std::map<size_t, uint64_t> gaps;
        for (const auto & gap : host.main.owned_gap_slices) {
            if (gap.owner_node_id != region.entry && !path.contains(gap.owner_node_id))
                throw std::runtime_error("Host removal would consume an external CPU gap");
            // A previously removed nested call leaves its original timestamps
            // apart, but its entry carries no remaining gap. Do not resurrect
            // that elapsed source interval as owned work of the outer call.
            if (!graph.node(gap.owner_node_id).cpu_gap_after && !graph.scope_gap_duration(gap.owner_node_id)) continue;
            gaps[gap.owner_node_id] += gap.owned_duration_us();
        }
        for (const auto id : path)
            if (gaps[id] != graph.node(id).cpu_gap_after || graph.scope_gap_duration(id))
                throw std::runtime_error("Host removal would erase a gap outside its owned interval: node=" + std::to_string(id)
                                         + " name=" + graph.event_for_node(id).name + " owned=" + std::to_string(gaps[id]) + " retained="
                                         + std::to_string(graph.node(id).cpu_gap_after) + " scoped=" + std::to_string(graph.scope_gap_duration(id))
                                         + " region=" + std::to_string(first.ts) + ".." + std::to_string(last.ts));
        if (gaps[region.entry] != graph.node(region.entry).cpu_gap_after || graph.scope_gap_duration(region.entry))
            throw std::runtime_error("Host entry gap is not wholly inside its call");
        removed.insert(path.begin(), path.end());
        plan.set_cpu_gaps.push_back({ .node_id = region.entry, .duration = 0 });
        additions.emplace(region.entry, region.exit, Kind::Sequential);
    }
    // Zero-duration enqueue leaves still submit real work. A cost template
    // omits them, so discover removal ownership from the complete CPU chain.
    const auto queues = simulation::detail::discover_cpu_task_queues(graph);
    std::vector<size_t> submitted(removed.begin(), removed.end());
    for (size_t i = 0; i < submitted.size(); ++i)
        for (const auto edge_id : source.outgoing_edge_ids(submitted[i])) {
            const auto & edge = graph.edge(edge_id);
            if (!edge.active || edge.kind != Kind::Correlation || removed.contains(edge.dst)) continue;
            const auto & node = graph.node(edge.dst);
            const auto task = node.is_cpu ? queues.node_task.at(edge.dst) : core::DagNode::kNoNode;
            if (!node.active || node.gpu_id != graph.node(submitted[i]).gpu_id
                || (node.is_cpu && (task == core::DagNode::kNoNode || !main.contains(queues.tasks[task].submission))))
                throw std::runtime_error("Host submission closure has no same-rank owned worker task: " + std::to_string(edge.src) + " "
                                         + graph.event_for_node(edge.src).name + " lane=" + std::string(graph.node_lane_key(edge.src))
                                         + " rank=" + std::to_string(graph.node(edge.src).gpu_id) + " -> " + std::to_string(edge.dst) + " "
                                         + graph.event_for_node(edge.dst).name + " lane=" + std::string(graph.node_lane_key(edge.dst))
                                         + " rank=" + std::to_string(node.gpu_id));
            removed.insert(edge.dst);
            if (node.is_cpu) workers.insert(edge.dst);
            submitted.push_back(edge.dst);
        }
    for (const auto & region : regions)
        for (const auto id : region.host->worker_nodes)
            if (!workers.contains(id)) throw std::runtime_error("Host worker has no owned submission path");
    for (const auto id : gates)
        if (removed.contains(id)) throw std::runtime_error("Host removal contains another region's gate");
    for (const auto id : workers) {
        const auto task = queues.node_task.at(id);
        if (task == core::DagNode::kNoNode || !removed.contains(queues.tasks.at(task).submission))
            throw std::runtime_error("Host removal worker lacks its owned submission");
    }
    for (size_t id = 0; id < queues.node_task.size(); ++id) {
        const auto task = queues.node_task[id];
        if (task != core::DagNode::kNoNode && removed.contains(queues.tasks[task].submission) && !workers.contains(id))
            throw std::runtime_error("Host removal owns only part of a worker task: " + std::to_string(id) + " " + graph.event_for_node(id).name);
    }
    // Only follow the resource's order, never an incoming event/record wait.
    const auto bypass = [&](size_t start, size_t consumer, Kind order, Kind output) {
        std::vector<size_t> pending{ start };
        std::set<size_t> visited;
        while (!pending.empty()) {
            const auto node = pending.back();
            pending.pop_back();
            if (!visited.insert(node).second) continue;
            for (const auto id : source.incoming_edge_ids(node)) {
                const auto & edge = graph.edge(id);
                if (!edge.active || edge.kind != order) continue;
                if (graph.node(edge.src).lane_id != graph.node(node).lane_id) throw std::runtime_error("Host removal order crosses resource lanes");
                if (removed.contains(edge.src)) pending.push_back(edge.src);
                else additions.emplace(edge.src, consumer, output);
            }
        }
    };
    for (const auto & edge : graph.edges()) {
        if (!edge.active || removed.contains(edge.src) == removed.contains(edge.dst)) continue;
        const bool outgoing = removed.contains(edge.src);
        const auto id = outgoing ? edge.src : edge.dst, peer = outgoing ? edge.dst : edge.src;
        if (const auto at = main.find(id); at != main.end()) {
            if (!outgoing && edge.kind == Kind::Sync && !graph.node(peer).is_cpu) continue;
            if (edge.kind != Kind::Sequential || peer != (outgoing ? at->second.second : at->second.first))
                throw std::runtime_error("Host removal has an external main-thread dependency");
        }
        else if (workers.contains(id)) {
            if (edge.kind != Kind::Sequential || graph.node(peer).lane_id != graph.node(id).lane_id)
                throw std::runtime_error("Host removal has an external worker dependency");
            if (outgoing) bypass(id, peer, Kind::Sequential, Kind::Sequential);
        }
        else if (outgoing) {
            if (edge.kind != Kind::Stream && (edge.kind != Kind::Sync || !core::synchronizes_device_frontier(graph.event_for_node(peer))))
                throw std::runtime_error("Removed device event still has an external event consumer: " + std::to_string(id) + " "
                                         + graph.event_for_node(id).name + " -> " + std::to_string(peer) + " " + graph.event_for_node(peer).name
                                         + " kind=" + std::to_string(static_cast<int>(edge.kind)));
            bypass(id, peer, Kind::Stream, edge.kind);
        }
        else if (edge.kind != Kind::Stream && edge.kind != Kind::Sync) { throw std::runtime_error("Removed device work has an external submission"); }
    }
    plan.disable_nodes.assign(removed.begin(), removed.end());
    for (const auto & [from, to, kind] : additions) {
        bool exists = false;
        for (const auto id : source.outgoing_edge_ids(from)) {
            const auto & edge = graph.edge(id);
            exists |= edge.active && edge.dst == to && edge.kind == kind;
        }
        if (!exists) plan.add_edges.push_back({ .src = core::DagNodeRef::existing(from), .dst = core::DagNodeRef::existing(to), .kind = kind });
    }
    return plan;
}

} // namespace markov::trace_graph::modules::hicache::runtime
