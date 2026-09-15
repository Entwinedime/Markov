#include "cpu_task_queues.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <tuple>

namespace markov::trace_graph::simulation::detail {
namespace {
constexpr auto none = core::DagNode::kNoNode;
using Identity = std::tuple<int, std::string, std::string>;
}

CpuTaskQueues discover_cpu_task_queues(const core::DagGraph& graph) {
    CpuTaskQueues result;
    result.node_task.assign(graph.node_count(), none);
    result.submitted_task.assign(graph.node_count(), none);
    std::map<Identity, std::vector<size_t>> submissions;
    std::map<size_t, std::vector<size_t>> lanes;
    const auto identity = [&](size_t id) {
        const auto& event = graph.event_for_node(id);
        return Identity{graph.node(id).gpu_id, event.pid, event.arg("correlation_id")};
    };
    const auto end = [&](size_t id) {
        const auto& event = graph.event_for_node(id);
        return core::checked_add_u64(event.ts, event.dur, "CPU task observation end overflow");
    };
    for (const auto& node : graph.nodes()) {
        if (!node.active || !node.is_cpu) continue;
        lanes[node.lane_id].push_back(node.id);
        const auto& event = graph.event_for_node(node.id);
        if (event.cat == "enqueue" && !event.arg("correlation_id").empty()) submissions[identity(node.id)].push_back(node.id);
    }
    for (auto& [lane, ids] : lanes) {
        std::ranges::sort(ids, {}, [&](size_t id) { return std::pair{graph.event_for_node(id).ts, id}; });
        std::vector<size_t> producers;
        for (const auto id : ids) {
            const auto found = submissions.find(identity(id));
            if (found == submissions.end() || found->second.size() != 1 || graph.node(found->second.front()).lane_id == lane) break;
            producers.push_back(found->second.front());
        }
        if (producers.size() != ids.size()) continue;
        std::vector<CpuTask> tasks;
        std::set<size_t> seen;
        for (size_t begin = 0; begin < ids.size();) {
            size_t stop = begin + 1;
            while (stop < ids.size() && producers[stop] == producers[begin]) ++stop;
            const auto submit = producers[begin], first = ids[begin], last = ids[stop - 1];
            const auto start = graph.event_for_node(first).ts;
            if (!seen.insert(submit).second || result.submitted_task[submit] != none
                || graph.event_for_node(submit).ts > start || (begin && end(ids[begin - 1]) > start)) break;
            const auto ready = std::max(end(submit), begin ? end(ids[begin - 1]) : uint64_t{0});
            tasks.push_back({first, last, submit, result.queue_count, start > ready ? start - ready : 0, ready > start ? ready - start : 0});
            begin = stop;
        }
        size_t covered = 0;
        for (const auto& task : tasks) {
            while (covered < ids.size() && producers[covered] == task.submission) ++covered;
        }
        if (covered != ids.size()) continue; // Partial identity is not a queue contract.
        const bool changed_cost = std::ranges::any_of(tasks, [&](const auto& task) {
            const auto& last = graph.node(task.last);
            return graph.scope_gap_duration(task.last) != 0 || (last.cpu_gap_after != 0 && last.cpu_gap_after != last.original_cpu_gap_after);
        });
        const bool cross_lane_delay = std::ranges::any_of(graph.edges(), [&](const auto& edge) {
            return edge.active && edge.kind == core::DagEdgeKind::Sequential && graph.node(edge.src).lane_id == lane
                   && graph.node(edge.dst).lane_id != lane;
        });
        if (changed_cost || cross_lane_delay) continue;
        ++result.queue_count;
        size_t offset = 0;
        for (const auto& task : tasks) {
            const auto task_id = result.tasks.size();
            result.submitted_task[task.submission] = task_id;
            while (offset < ids.size() && producers[offset] == task.submission) result.node_task[ids[offset++]] = task_id;
            result.tasks.push_back(task);
        }
    }
    return result;
}

bool CpuTaskQueues::replaces(const core::DagEdge& edge) const {
    if (tasks.empty() || edge.kind != core::DagEdgeKind::Sequential) return false;
    const auto from = node_task.at(edge.src), to = node_task.at(edge.dst);
    return from != none && to != none && from != to && tasks[from].queue == tasks[to].queue;
}

void CpuTaskQueues::materialize(core::DagGraph& graph, const std::vector<std::vector<size_t>>& order) const {
    if (tasks.empty()) return;
    std::vector<size_t> desired(graph.node_count(), none);
    std::vector<bool> retained(graph.node_count(), false);
    for (const auto& queue : order) {
        for (size_t index = 1; index < queue.size(); ++index) desired[tasks[queue[index]].first] = tasks[queue[index - 1]].last;
    }
    for (size_t id = 0; id < graph.edge_count(); ++id) {
        const auto& edge = graph.edge(id);
        if (!edge.active || !replaces(edge)) continue;
        if (desired[edge.dst] == edge.src && !retained[edge.dst]) retained[edge.dst] = true;
        else graph.disable_edge(id);
    }
    for (const auto& task : tasks) {
        graph.mutable_node(task.first).cpu_ready_delay_before = task.ready_delay_us;
        graph.mutable_node(task.last).cpu_gap_after = 0;
    }
    for (const auto& task : tasks) if (desired[task.first] != none && !retained[task.first])
        graph.add_edge(desired[task.first], task.first, core::DagEdgeKind::Sequential);
}

} // namespace markov::trace_graph::simulation::detail
