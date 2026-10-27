#include "markov/trace_graph/modules/hicache/runtime/host_removal.hpp"
#include "markov/trace_graph/adapters/frameworks/sglang/hicache_observations.hpp"
#include "markov/trace_graph/core/dag_builder.hpp"
#include "markov/trace_graph/simulation/cpu_task_queues.hpp"
#include "markov/trace_graph/source_dag/call_regions.hpp"
#include <algorithm>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace markov::trace_graph::modules::hicache::runtime {

core::DagNodeRef append_host_entry_gate(const adapters::sglang::HiCacheSourceIndex & source, size_t first, core::DagObservedPoint point, std::string id,
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

core::DagMutationPlan plan_host_removal(const adapters::sglang::HiCacheSourceIndex & source, std::span<const HiCacheHostRegion> regions,
                                        std::span<const size_t> collective_handoffs, std::span<const core::DagRedirectEdgeMutation> submission_handoffs) {
    const auto queues = simulation::detail::discover_cpu_task_queues(source.graph());
    return plan_host_removal(source, queues, regions, collective_handoffs, submission_handoffs);
}

core::DagMutationPlan plan_host_removal(const adapters::sglang::HiCacheSourceIndex & source, const simulation::detail::CpuTaskQueues & queues,
                                        std::span<const HiCacheHostRegion> regions, std::span<const size_t> collective_handoffs,
                                        std::span<const core::DagRedirectEdgeMutation> submission_handoffs) {
    using Kind = core::DagEdgeKind;
    const auto & graph = source.graph();
    core::DagMutationPlan plan{ .component = "hicache_host_replacement" };
    std::set<size_t> removed, workers, gates, opening_submissions;
    const std::set<size_t> transferred_collectives(collective_handoffs.begin(), collective_handoffs.end());
    std::set<size_t> transferred_submissions;
    std::set<size_t> transferred_submission_nodes;
    for (const auto & handoff : submission_handoffs) {
        const auto & edge = graph.edge(handoff.edge_index);
        if (!edge.active || edge.kind != Kind::Correlation || !handoff.src || handoff.dst || !graph.node(edge.src).is_cpu || !graph.node(edge.dst).is_cpu)
            throw std::runtime_error("Caller handoff must preserve a CPU task and replace only its submission endpoint");
        transferred_submissions.insert(handoff.edge_index);
        transferred_submission_nodes.insert(edge.src);
        plan.redirect_edges.push_back(handoff);
    }
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
            // Count only retained source pieces, exactly as host expansion
            // does. Nested replacements may have removed all or part of the
            // old timestamp gap before this enclosing call is replaced.
            const auto retained = graph.observed_cpu_gap_ranges(gap.owner_node_id);
            if (!retained) throw std::runtime_error("Host removal cannot own a remodeled gap without source coordinates");
            for (const auto & [lo, hi] : *retained) {
                const auto begin = std::max(lo, gap.owned_start_us), end = std::min(hi, gap.owned_end_us);
                if (begin < end) gaps[gap.owner_node_id] += end - begin;
            }
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
        for (const auto node : region.opening_nodes) {
            removed.insert(node);
            if (graph.node(node).is_cpu) opening_submissions.insert(node);
        }
        plan.set_cpu_gaps.push_back({ .node_id = region.entry, .duration = 0 });
        additions.emplace(region.entry, region.exit, Kind::Sequential);
    }
    // Discover removal ownership from the complete CPU chain, including
    // zero-duration enqueue points and nested replacement gates.
    std::vector<size_t> submitted(removed.begin(), removed.end());
    for (size_t i = 0; i < submitted.size(); ++i)
        for (const auto edge_id : source.outgoing_edge_ids(submitted[i])) {
            if (transferred_submissions.contains(edge_id)) continue;
            const auto & edge = graph.edge(edge_id);
            if (!edge.active || removed.contains(edge.dst)) continue;
            const auto source_task = queues.node_task.at(edge.src);
            const bool same_task = edge.kind == Kind::Sequential && source_task != core::DagNode::kNoNode && queues.node_task.at(edge.dst) == source_task
                                   && removed.contains(queues.tasks.at(source_task).submission);
            if (edge.kind != Kind::Correlation && !same_task) continue;

            // A submitted CPU task can contain multiple runtime spans. Its
            // continuation belongs to this call even without a second
            // Correlation edge. Never follow the FIFO into the next task.
            const auto & node = graph.node(edge.dst);
            const auto task = node.is_cpu ? queues.node_task.at(edge.dst) : core::DagNode::kNoNode;
            if (!node.active || node.gpu_id != graph.node(submitted[i]).gpu_id
                || (node.is_cpu
                    && (task == core::DagNode::kNoNode
                        || (!main.contains(queues.tasks[task].submission) && !opening_submissions.contains(queues.tasks[task].submission)))))
                throw std::runtime_error("Host submission closure has no same-rank owned worker task: " + std::to_string(edge.src) + " "
                                         + graph.event_for_node(edge.src).name + " lane=" + std::string(graph.node_lane_key(edge.src))
                                         + " rank=" + std::to_string(graph.node(edge.src).gpu_id) + " -> " + std::to_string(edge.dst) + " "
                                         + graph.event_for_node(edge.dst).name + " lane=" + std::string(graph.node_lane_key(edge.dst))
                                         + " rank=" + std::to_string(node.gpu_id));
            removed.insert(edge.dst);
            if (node.is_cpu) workers.insert(edge.dst);
            submitted.push_back(edge.dst);
        }
    const auto placeholders = source_dag::interior_stream_placeholders(source, removed);
    removed.insert(placeholders.begin(), placeholders.end());
    plan.disable_nodes.assign(removed.begin(), removed.end());
    // Ownership is complete now. Index the final removal list once: auditing
    // every graph edge must not repeat tree searches through millions of IDs.
    // The sorted plan remains the mutation's canonical removal order.
    const auto removed_nodes = [&] {
        std::vector<bool> nodes(graph.node_count());
        for (const auto id : plan.disable_nodes) nodes[id] = true;

        return nodes;
    }();

    for (const auto & region : regions)
        for (const auto id : region.host->worker_nodes)
            if (!workers.contains(id)) throw std::runtime_error("Host worker has no owned submission path");
    for (const auto id : gates)
        if (removed_nodes[id]) throw std::runtime_error("Host removal contains another region's gate");
    for (const auto id : transferred_submissions) {
        const auto & edge = graph.edge(id);
        if (!main.contains(edge.src) || removed_nodes[edge.dst]) throw std::runtime_error("Caller handoff must leave its complete worker task outside removal");
    }
    for (const auto id : workers) {
        const auto task = queues.node_task.at(id);
        if (task == core::DagNode::kNoNode || !removed_nodes[queues.tasks.at(task).submission])
            throw std::runtime_error("Host removal worker lacks its owned submission");
        if (transferred_submission_nodes.contains(queues.tasks.at(task).submission))
            throw std::runtime_error("Caller handoff cannot remove part of its retained worker task");
    }
    for (size_t id = 0; id < queues.node_task.size(); ++id) {
        const auto task = queues.node_task[id];
        if (task != core::DagNode::kNoNode && removed_nodes[queues.tasks[task].submission]
            && !transferred_submission_nodes.contains(queues.tasks[task].submission) && !workers.contains(id))
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
                if (removed_nodes[edge.src]) pending.push_back(edge.src);
                else additions.emplace(edge.src, consumer, output);
            }
        }
    };
    for (size_t edge_id = 0; edge_id < graph.edges().size(); ++edge_id) {
        const auto & edge = graph.edge(edge_id);
        if (!edge.active || removed_nodes[edge.src] == removed_nodes[edge.dst]) continue;
        const bool outgoing = removed_nodes[edge.src];
        const auto id = outgoing ? edge.src : edge.dst, peer = outgoing ? edge.dst : edge.src;
        if (const auto at = main.find(id); at != main.end()) {
            if (outgoing && transferred_submissions.contains(edge_id)) continue;
            if (!outgoing && edge.kind == Kind::Sync && !graph.node(peer).is_cpu) continue;
            if (edge.kind != Kind::Sequential || peer != (outgoing ? at->second.second : at->second.first))
                throw std::runtime_error("Host removal has an external main-thread dependency");
        }
        else if (opening_submissions.contains(id)) {
            if (edge.kind != Kind::Sequential || graph.node(peer).lane_id != graph.node(id).lane_id)
                throw std::runtime_error("Opening submission has an external nonqueue dependency");
            if (outgoing) bypass(id, peer, Kind::Sequential, Kind::Sequential);
        }
        else if (workers.contains(id)) {
            if (edge.kind != Kind::Sequential || graph.node(peer).lane_id != graph.node(id).lane_id)
                throw std::runtime_error("Host removal has an external worker dependency");
            if (outgoing) bypass(id, peer, Kind::Sequential, Kind::Sequential);
        }
        else if (outgoing) {
            if (edge.kind != Kind::Stream && (edge.kind != Kind::Sync || !core::synchronizes_device_frontier(graph.event_for_node(peer))))
                throw std::runtime_error("Removed device event still has an external event consumer: " + std::to_string(id) + " "
                                         + graph.event_for_node(id).name + " -> " + std::to_string(peer) + " " + graph.event_for_node(peer).name + " kind="
                                         + std::to_string(static_cast<int>(edge.kind)) + " source_ts_us=" + std::to_string(graph.event_for_node(id).ts)
                                         + " consumer_ts_us=" + std::to_string(graph.event_for_node(peer).ts)
                                         + " consumer_pid=" + graph.event_for_node(peer).pid + " consumer_tid=" + graph.event_for_node(peer).tid);
            bypass(id, peer, Kind::Stream, edge.kind);
        }
        else if (edge.kind != Kind::Stream && edge.kind != Kind::Sync
                 && (edge.kind != Kind::HCCL || graph.node(peer).is_cpu || !transferred_collectives.contains(edge_id))) {
            throw std::runtime_error("Removed device work has an external submission");
        }
    }
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
