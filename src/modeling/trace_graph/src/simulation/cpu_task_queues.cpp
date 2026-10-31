#include "markov/trace_graph/simulation/cpu_task_queues.hpp"
#include "core/dag_index.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace markov::trace_graph::simulation::detail {
namespace {
constexpr auto none = core::DagNode::kNoNode;

std::optional<core::CpuTaskObservations::QueueProgram> bind_source_program(const core::CpuTaskObservations::QueueProgram & source, const core::DagGraph & graph,
                                                                           std::span<const size_t> submissions) {
    size_t member_count = 0, task_count = 0;
    for (const auto & task : source.tasks) {
        size_t active = 0;
        for (size_t index = task.member_begin; index < task.member_end; ++index) active += graph.node(source.members[index]).active;
        if (!active) continue;
        if (active != task.member_end - task.member_begin || submissions[task.identity] != task.submission) return std::nullopt;
        const auto & node = graph.node(task.submission);
        if (!graph.source_event_unchanged(node.event_index)) {
            const auto & event = graph.event_for_node(task.submission);
            if (core::checked_add_u64(event.ts, event.dur, "CPU submission end overflow") != task.submission_end_us) return std::nullopt;
        }
        member_count += active;
        ++task_count;
    }

    core::CpuTaskObservations::QueueProgram bound;
    bound.complete = true;
    bound.members.reserve(member_count);
    bound.tasks.reserve(task_count);
    for (auto task : source.tasks) {
        if (!graph.node(task.first).active) continue;
        const auto first = source.members.begin() + task.member_begin;
        const auto last = source.members.begin() + task.member_end;
        task.member_begin = bound.members.size();
        bound.members.insert(bound.members.end(), first, last);
        task.member_end = bound.members.size();
        bound.tasks.push_back(task);
    }
    return bound;
}
} // namespace

CpuTaskQueues discover_cpu_task_queues(const core::DagGraph & graph) {
    CpuTaskQueues result;
    result.node_task.assign(graph.node_count(), none);
    result.submitted_task.assign(graph.node_count(), none);
    const auto observed_source = graph.cpu_task_observations();
    const auto & source = *observed_source;
    std::map<core::CpuTaskObservations::Identity, size_t> added_identities;
    std::vector<std::pair<size_t, size_t>> added_submissions;
    std::map<size_t, std::vector<core::CpuTaskObservations::Member>> added_members;
    std::set<size_t> lanes;
    for (const auto & [lane, members] : source.lanes) lanes.insert(lane);
    std::map<size_t, std::vector<size_t>> modeled_tasks, modeled_submissions;
    std::map<size_t, std::vector<size_t>> modeled_successors;
    std::set<size_t> changed_source_tasks;
    const auto identity_for = [&](const core::DagNode & node, const core::TraceEvent & event) {
        const auto correlation = event.arg("correlation_id");
        if (correlation.empty()) return none;
        const core::CpuTaskObservations::Identity key{ node.gpu_id, event.pid, correlation };
        const auto found = source.identities.find(key);
        return found != source.identities.end() ? found->second
                                                : added_identities.try_emplace(key, source.identities.size() + added_identities.size()).first->second;
    };
    for (const auto & node : graph.nodes()) {
        if (!node.is_cpu) continue;
        if (node.explicit_cpu_task) {
            if (node.id < source.node_count) changed_source_tasks.insert(node.lane_id);
            if (node.active) {
                lanes.insert(node.lane_id);
                modeled_tasks[node.lane_id].push_back(node.id);
            }
            continue; // No source timestamps: do not change observed predecessors.
        }
        if (node.id < source.node_count && graph.source_event_unchanged(node.event_index)) continue;

        const auto & event = graph.event_for_node(node.id);
        const auto identity = identity_for(node, event);
        if (identity != none && event.cat == "enqueue") added_submissions.emplace_back(identity, node.id);
        lanes.insert(node.lane_id);
        added_members[node.lane_id].push_back({ node.id, identity, event.ts, core::checked_add_u64(event.ts, event.dur, "CPU task observation end overflow") });
    }
    // Only observed facts are shared. Active submissions, costs and edited dependencies belong to this graph.
    constexpr auto ambiguous = none - 1;
    std::vector<size_t> submissions(source.identities.size() + added_identities.size(), none);
    const auto submit = [&](const auto & entries, bool original) {
        for (const auto & [identity, node] : entries) {
            if (!graph.node(node).active || graph.node(node).explicit_cpu_task) continue;
            if (original && !graph.source_event_unchanged(graph.node(node).event_index)) continue;
            auto & unique = submissions[identity];
            unique = unique == none ? node : ambiguous;
        }
    };
    submit(source.submissions, true);
    submit(added_submissions, false);

    // Inspect dependencies once, not once per CPU lane in a potentially large DAG.
    std::set<size_t> cross_lane_delays;
    const auto inspect_edge = [&](size_t edge_id) {
        const auto & edge = graph.edge(edge_id);
        if (!edge.active) return;

        if (edge.kind == core::DagEdgeKind::Sequential && graph.node(edge.src).lane_id != graph.node(edge.dst).lane_id)
            cross_lane_delays.insert(graph.node(edge.src).lane_id);

        if (!modeled_tasks.empty() && edge.kind == core::DagEdgeKind::Correlation && graph.node(edge.dst).explicit_cpu_task && graph.node(edge.dst).active
            && graph.node(edge.src).active)
            modeled_submissions[edge.dst].push_back(edge.src);
        if (edge.kind == core::DagEdgeKind::Sequential && graph.node(edge.src).explicit_cpu_task && graph.node(edge.dst).explicit_cpu_task
            && graph.node(edge.src).active && graph.node(edge.dst).active)
            modeled_successors[edge.src].push_back(edge.dst);
    };
    for (const auto edge : source.task_edges) inspect_edge(edge);
    for (size_t edge = source.edge_count; edge < graph.edge_count(); ++edge) inspect_edge(edge);

    for (const auto lane : lanes) {
        std::span<const core::CpuTaskObservations::Member> observed;
        if (const auto found = source.lanes.find(lane); found != source.lanes.end()) observed = found->second;
        if (const auto found = added_members.find(lane); found != added_members.end()) {
            auto & merged = found->second;
            for (const auto & member : observed)
                if (graph.source_event_unchanged(graph.node(member.node).event_index)) merged.push_back(member);
            std::ranges::sort(merged,
                              [](const auto & left, const auto & right) { return std::tie(left.start_us, left.node) < std::tie(right.start_us, right.node); });
            observed = merged;
        }
        const auto & additions = modeled_tasks[lane];
        const auto reject_insertion = [&] {
            if (!additions.empty()) throw std::runtime_error("cannot insert CPU task into an unrecognized worker queue");
        };
        // Whole native tasks reuse their compiled source grouping. Partial
        // removal or event edits use the same inference on the revised lane.
        std::optional<core::CpuTaskObservations::QueueProgram> reused;
        const auto compiled = source.queue_programs.find(lane);
        if (compiled != source.queue_programs.end() && !added_members.contains(lane) && !changed_source_tasks.contains(lane))
            reused = bind_source_program(compiled->second, graph, submissions);
        auto program = reused ? std::move(*reused) : core::compile_cpu_queue_program(graph, observed, submissions);
        const bool duplicate_submission = std::ranges::any_of(program.tasks, [&](const auto & task) { return result.submitted_task[task.submission] != none; });
        if (!program.complete || duplicate_submission) {
#ifdef DEBUG
            if (program.missing_submission) {
                const auto unresolved = *program.missing_submission;
                const auto & event = graph.event_for_node(unresolved);
                result.unavailable_lanes[lane] = "No unique cross-thread enqueue for node=" + std::to_string(unresolved) + " name=" + event.name
                                                 + " ts_us=" + std::to_string(event.ts) + " correlation=" + event.arg("correlation_id");
            }
            else result.unavailable_lanes[lane] = "Task ordering or submission identity conflicts with its worker queue";
#endif
            reject_insertion();
            continue;
        }
        if (program.tasks.empty() && additions.empty()) {
            if (compiled != source.queue_programs.end()) {
                const auto rank = graph.node(compiled->second.tasks.front().first).gpu_id;
                result.worker_queues.emplace(std::string(graph.lane_key(lane)), CpuTaskQueues::WorkerResource{ result.queue_count++, rank });
            }
            continue;
        }
        std::vector<CpuTask> tasks;
        tasks.reserve(program.tasks.size());
        for (const auto & task : program.tasks)
            tasks.push_back({ task.first, task.last, task.submission, result.queue_count, task.ready_delay_us, task.submission_overlap_us });
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
        for (const auto & task : tasks) result.submitted_task[task.submission] = ambiguous;
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
            if (result.submitted_task[submit] != none) throw std::runtime_error("one CPU submission cannot identify multiple queue tasks");

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
        result.worker_queues.emplace(std::string(graph.lane_key(lane)),
                                     CpuTaskQueues::WorkerResource{ result.queue_count++, graph.node(tasks.front().first).gpu_id });
        if (reused) result.source_program_tasks_reused += program.tasks.size();
        for (size_t index = 0; index < tasks.size(); ++index) {
            const auto & task = tasks[index];
            const auto task_id = result.tasks.size();
            result.submitted_task[task.submission] = task_id;
            if (index < program.tasks.size()) {
                const auto & native = program.tasks[index];
                for (size_t member = native.member_begin; member < native.member_end; ++member) result.node_task[program.members[member]] = task_id;
            }
            else {
                for (const auto member : modeled_jobs.at(task.submission)) result.node_task[member] = task_id;
            }
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
