#include "markov/trace_graph/simulation/cpu_task_queues.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace markov::trace_graph::simulation::detail {
namespace {
constexpr auto none = core::DagNode::kNoNode;
using Identity = std::tuple<int, std::string, std::string>;
} // namespace

CpuTaskQueues discover_cpu_task_queues(const core::DagGraph & graph) {
    CpuTaskQueues result;
    result.node_task.assign(graph.node_count(), none);
    result.submitted_task.assign(graph.node_count(), none);
    std::map<Identity, std::vector<size_t>> submissions;
    std::map<size_t, std::vector<std::pair<uint64_t, size_t>>> lanes;
    std::map<size_t, std::vector<size_t>> modeled_tasks, modeled_submissions;
    std::map<size_t, std::vector<size_t>> modeled_successors;
    const auto identity = [&](size_t id) {
        const auto & event = graph.event_for_node(id);
        return Identity{ graph.node(id).gpu_id, event.pid, event.arg("correlation_id") };
    };
    const auto end = [&](size_t id) {
        const auto & event = graph.event_for_node(id);
        return core::checked_add_u64(event.ts, event.dur, "CPU task observation end overflow");
    };
    for (const auto & node : graph.nodes()) {
        if (!node.is_cpu) continue;
        if (node.explicit_cpu_task) {
            if (node.active) {
                lanes.try_emplace(node.lane_id);
                modeled_tasks[node.lane_id].push_back(node.id);
            }
            continue; // No source timestamps: do not change observed predecessors.
        }
        const auto & event = graph.event_for_node(node.id);
        lanes[node.lane_id].emplace_back(event.ts, node.id);
        if (!node.active) continue;
        if (event.cat == "enqueue" && !event.arg("correlation_id").empty()) submissions[identity(node.id)].push_back(node.id);
    }
    // Inspect dependencies once, not once per CPU lane in a potentially large DAG.
    std::set<size_t> cross_lane_delays;
    for (const auto & edge : graph.edges()) {
        if (!edge.active) continue;

        if (edge.kind == core::DagEdgeKind::Sequential && graph.node(edge.src).lane_id != graph.node(edge.dst).lane_id)
            cross_lane_delays.insert(graph.node(edge.src).lane_id);

        if (!modeled_tasks.empty() && edge.kind == core::DagEdgeKind::Correlation && graph.node(edge.dst).explicit_cpu_task && graph.node(edge.dst).active
            && graph.node(edge.src).active)
            modeled_submissions[edge.dst].push_back(edge.src);
        if (edge.kind == core::DagEdgeKind::Sequential && graph.node(edge.src).explicit_cpu_task && graph.node(edge.dst).explicit_cpu_task
            && graph.node(edge.src).active && graph.node(edge.dst).active)
            modeled_successors[edge.src].push_back(edge.dst);
    }

    for (auto & [lane, observed] : lanes) {
        const auto & additions = modeled_tasks[lane];
        const auto reject_insertion = [&] {
            if (!additions.empty()) throw std::runtime_error("cannot insert CPU task into an unrecognized worker queue");
        };
        // Sort compact coordinates rather than dereferencing the full trace
        // for every comparison. Timestamp/ID ordering is unchanged.
        if (!std::ranges::is_sorted(observed)) std::ranges::sort(observed);

        // Task removal changes the queue, not the source-measured ready delay.
        // Otherwise a removed task's service is charged again as its successor's gap.
        std::vector<size_t> ids;
        std::vector<uint64_t> observed_previous_ends;
        ids.reserve(observed.size());
        observed_previous_ends.reserve(observed.size());
        uint64_t previous_end = 0;
        for (const auto & [at, id] : observed) {
            if (graph.node(id).active) {
                ids.push_back(id);
                observed_previous_ends.push_back(previous_end);
            }
            previous_end = end(id);
        }
        if (ids.empty() && additions.empty()) continue;
        std::vector<size_t> producers;
        for (const auto id : ids) {
            const auto found = submissions.find(identity(id));
            if (found == submissions.end() || found->second.size() != 1 || graph.node(found->second.front()).lane_id == lane) break;
            producers.push_back(found->second.front());
        }
        if (producers.size() != ids.size()) {
#ifdef DEBUG
            const auto unresolved = ids[producers.size()];
            const auto & event = graph.event_for_node(unresolved);
            result.unavailable_lanes[lane] = "No unique cross-thread enqueue for node=" + std::to_string(unresolved) + " name=" + event.name
                                             + " ts_us=" + std::to_string(event.ts) + " correlation=" + event.arg("correlation_id");
#endif
            reject_insertion();
            continue;
        }
        std::vector<CpuTask> tasks;
        std::set<size_t> seen;
        for (size_t begin = 0; begin < ids.size();) {
            size_t stop = begin + 1;
            while (stop < ids.size() && producers[stop] == producers[begin]) ++stop;
            const auto submit = producers[begin], first = ids[begin], last = ids[stop - 1];
            const auto start = graph.event_for_node(first).ts;
            if (!seen.insert(submit).second || result.submitted_task[submit] != none || graph.event_for_node(submit).ts > start
                || observed_previous_ends[begin] > start)
                break;
            const auto ready = std::max(end(submit), observed_previous_ends[begin]);
            tasks.push_back({ first, last, submit, result.queue_count, start > ready ? start - ready : 0, ready > start ? ready - start : 0 });
            begin = stop;
        }
        size_t covered = 0;
        for (const auto & task : tasks) {
            while (covered < ids.size() && producers[covered] == task.submission) ++covered;
        }
        if (covered != ids.size()) {
#ifdef DEBUG
            result.unavailable_lanes[lane] =
                "Task ordering or submission identity failed after " + std::to_string(covered) + " of " + std::to_string(ids.size()) + " observed members";
#endif
            reject_insertion();
            continue;
        } // Partial identity is not a queue contract.
        const bool changed_cost = std::ranges::any_of(tasks, [&](const auto & task) {
            const auto & last = graph.node(task.last);
            return graph.scope_gap_duration(task.last) != 0 || (last.cpu_gap_after != 0 && last.cpu_gap_after != last.original_cpu_gap_after);
        });
        if (changed_cost || cross_lane_delays.contains(lane)) {
#ifdef DEBUG
            result.unavailable_lanes[lane] = changed_cost ? "Queue contains a remodeled sequential gap" : "Queue has an external Sequential dependency";
#endif
            reject_insertion();
            continue;
        }
        std::map<size_t, std::vector<size_t>> modeled_jobs;
        for (const auto id : additions) {
            const auto & incoming = modeled_submissions[id];
            if (incoming.size() != 1 || !graph.node(incoming.front()).is_cpu || graph.node(incoming.front()).lane_id == lane)
                throw std::runtime_error("explicit CPU task requires one cross-thread CPU submission");
            if (graph.node(id).cpu_gap_after != 0 || graph.scope_gap_duration(id) != 0)
                throw std::runtime_error("explicit CPU task cannot carry an unmodeled sequential gap");
            modeled_jobs[incoming.front()].push_back(id);
        }
        for (const auto & [submit, members] : modeled_jobs) {
            if (!seen.insert(submit).second || result.submitted_task[submit] != none)
                throw std::runtime_error("one CPU submission cannot identify multiple queue tasks");

            // A generated task may contain several CPU members. Their
            // internal Sequential chain, not insertion order or timestamps,
            // determines its first/last member. FIFO edges to other tasks
            // are scheduling results and do not join these identities.
            const std::set<size_t> owned(members.begin(), members.end());
            std::map<size_t, size_t> next, previous;
            for (const auto id : members)
                for (const auto successor : modeled_successors[id]) {
                    if (!owned.contains(successor)) continue;
                    if (!next.emplace(id, successor).second || !previous.emplace(successor, id).second)
                        throw std::runtime_error("explicit CPU task members must form one unbranched sequence");
                }
            std::vector<size_t> first;
            for (const auto id : members)
                if (!previous.contains(id)) first.push_back(id);
            if (first.size() != 1) throw std::runtime_error("explicit CPU task has no unique entry");

            auto last = first.front();
            size_t covered_members = 1;
            while (next.contains(last) && covered_members < members.size()) {
                last = next.at(last);
                ++covered_members;
                if (graph.node(last).cpu_ready_delay_before) throw std::runtime_error("only the first CPU task member may carry a dispatch delay");
            }
            if (covered_members != members.size() || next.contains(last)) throw std::runtime_error("explicit CPU task is disconnected or cyclic");
            tasks.push_back({ first.front(), last, submit, result.queue_count, graph.node(first.front()).cpu_ready_delay_before, 0 });
        }
        ++result.queue_count;
        size_t offset = 0;
        for (const auto & task : tasks) {
            const auto task_id = result.tasks.size();
            result.submitted_task[task.submission] = task_id;
            while (offset < ids.size() && producers[offset] == task.submission) result.node_task[ids[offset++]] = task_id;
            if (graph.node(task.first).explicit_cpu_task)
                for (const auto member : modeled_jobs.at(task.submission)) result.node_task[member] = task_id;
            result.tasks.push_back(task);
        }
    }
    return result;
}

bool CpuTaskQueues::replaces(const core::DagEdge & edge) const {
    if (tasks.empty() || edge.kind != core::DagEdgeKind::Sequential) return false;
    const auto from = node_task.at(edge.src), to = node_task.at(edge.dst);
    return from != none && to != none && from != to && tasks[from].queue == tasks[to].queue;
}

void CpuTaskQueues::materialize(core::DagGraph & graph, const std::vector<std::vector<size_t>> & order) const {
    if (tasks.empty()) return;
    std::vector<size_t> desired(graph.node_count(), none);
    std::vector<bool> retained(graph.node_count(), false);
    for (const auto & queue : order) {
        for (size_t index = 1; index < queue.size(); ++index) desired[tasks[queue[index]].first] = tasks[queue[index - 1]].last;
    }
    for (size_t id = 0; id < graph.edge_count(); ++id) {
        const auto & edge = graph.edge(id);
        if (!edge.active || !replaces(edge)) continue;
        if (desired[edge.dst] == edge.src && !retained[edge.dst]) retained[edge.dst] = true;
        else graph.disable_edge(id);
    }
    for (const auto & task : tasks) {
        graph.mutable_node(task.first).cpu_ready_delay_before = task.ready_delay_us;
        graph.mutable_node(task.last).cpu_gap_after = 0;
    }
    for (const auto & task : tasks)
        if (desired[task.first] != none && !retained[task.first]) graph.add_edge(desired[task.first], task.first, core::DagEdgeKind::Sequential);
}

} // namespace markov::trace_graph::simulation::detail
