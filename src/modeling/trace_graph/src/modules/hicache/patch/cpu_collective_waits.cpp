#include "markov/trace_graph/modules/hicache/patch/cpu_collective_waits.hpp"
#include "markov/trace_graph/modules/hicache/cpu_collective_timing.hpp"
#include <algorithm>
#include <set>
#include <tuple>

namespace markov::trace_graph::modules::hicache::patch {
namespace {
using Ref = core::DagNodeRef;
using Kind = core::DagEdgeKind;
struct Point {
    uint64_t time;
    size_t node;
    int order;
};
struct Gap {
    size_t edge;
    uint64_t begin, end;
    std::vector<Point> points;
    std::vector<std::pair<uint64_t, uint64_t>> cuts;
};

class Planner {
public:
    explicit Planner(const HiCacheSourceDagIndex & source) : source(source), graph(source.graph()) {}
    CpuCollectiveWaitPlan run(const std::vector<const CpuCollectiveRound *> & rounds) {
        check_call_order(rounds);
        for (const auto * round : rounds) add_round(*round);
        for (auto & [owner, gap] : gaps) splice(owner, gap);
        for (const auto & [node, duration] : cpu_gaps)
            result.mutation.set_cpu_gaps.push_back({ .node_id = node, .duration = duration, .effect_id = "cpu_collective" });
        if (!result.issues.empty()) result.mutation = { .component = "cpu_collective" };
        return std::move(result);
    }

private:
    const HiCacheSourceDagIndex & source;
    const core::DagGraph & graph;
    CpuCollectiveWaitPlan result;
    std::map<size_t, Gap> gaps;
    std::map<size_t, uint64_t> cpu_gaps;
    std::set<size_t> claimed;

    void issue(const char * reason) { ++result.issues[reason]; }
    void check_call_order(const std::vector<const CpuCollectiveRound *> & rounds) {
        std::set<std::string> groups;
        for (const auto * round : rounds) groups.insert(round->group);
        std::map<std::pair<std::string, std::string>, std::vector<const core::TraceEvent *>> calls;
        for (const auto & event : graph.runtime_observations())
            if (event.name == "runtime.cpu_collective" && groups.contains(event.arg("group")))
                calls[{ event.arg("group"), event.arg("rank") }].push_back(&event);
        for (auto & [key, events] : calls) {
            std::ranges::sort(events, {}, [](const auto * event) { return event->ts; });
            uint64_t end = 0;
            for (const auto * event : events) {
                if (event->pid != events.front()->pid || event->tid != events.front()->tid || event->ts < end || event->arg("async_op") != "false"
                    || event->arg("status") != "returned")
                    issue("group_is_not_single_caller_synchronous");
                end = event->ts + event->dur;
            }
        }
    }
    Ref ref(size_t node) const { return Ref::synthetic(result.mutation.synthetic_nodes[node].synthetic_id); }
    size_t marker(const std::string & id, const std::string & lane, uint64_t gap = 0) {
        const auto node = result.mutation.synthetic_nodes.size();
        result.mutation.synthetic_nodes.push_back({
            .synthetic_id = id,
            .node = { .name = id, .lane_key = lane, .cpu_gap_after = gap },
            .effect_id = "cpu_collective"
        });
        return node;
    }
    void connect(Ref from, Ref to, Kind kind = Kind::Sync) {
        result.mutation.add_edges.push_back({ .src = std::move(from), .dst = std::move(to), .kind = kind, .effect_id = "cpu_collective" });
    }
    Gap * gap_after(size_t owner) {
        if (auto at = gaps.find(owner); at != gaps.end()) return &at->second;
        std::vector<size_t> edges;
        for (const auto id : source.outgoing_edge_ids(owner))
            if (graph.edge(id).kind == Kind::Sequential) edges.push_back(id);
        if (edges.size() != 1) {
            issue("nonunique_cpu_successor");
            return nullptr;
        }
        const auto & edge = graph.edge(edges.front());
        const auto & before = graph.event_for_node(owner);
        const auto & after = graph.event_for_node(edge.dst);
        const auto begin = before.ts + before.dur;
        if (graph.node(owner).lane_id != graph.node(edge.dst).lane_id || after.ts < begin || graph.node(owner).cpu_gap_after != after.ts - begin
            || graph.scope_gap_duration(owner) != 0) {
            issue("cpu_gap_already_changed_or_overlapping");
            return nullptr;
        }
        return &gaps.emplace(owner, Gap{ edges.front(), begin, after.ts, {}, {} }).first->second;
    }
    Gap * gap_before(size_t successor) {
        std::vector<size_t> owners;
        for (const auto id : source.incoming_edge_ids(successor))
            if (graph.edge(id).kind == Kind::Sequential) owners.push_back(graph.edge(id).src);
        if (owners.size() != 1) {
            issue("nonunique_cpu_predecessor");
            return nullptr;
        }
        return gap_after(owners.front());
    }
    void add_round(const CpuCollectiveRound & round) {
        const auto timing = observe_cpu_collective_timing(graph, round);
        if (!timing.issue.empty()) {
            issue(timing.issue.c_str());
            return;
        }
        std::map<int, size_t> arrivals;
        for (const auto & call : round.calls) {
            const auto entry = add_call(call, timing.calls.at(call.rank));
            if (entry) arrivals[call.rank] = *entry;
        }
        if (arrivals.size() != round.calls.size()) return;
        for (const auto & call : round.calls)
            for (const auto peer : timing.calls.at(call.rank).entry_ranks) connect(ref(arrivals.at(peer)), Ref::existing(*call.worker));
    }
    std::optional<size_t> add_call(const CpuCollectiveCall & call, const CpuCollectiveCallTiming & timing) {
        const auto & event = graph.runtime_observations()[call.observation];
        const auto & submit = graph.event_for_node(*call.submission);
        const auto & worker = graph.event_for_node(*call.worker);
        const auto finish = event.ts + event.dur, submit_end = submit.ts + submit.dur;
        if (!claimed.insert(*call.submission).second || !claimed.insert(*call.worker).second) {
            issue("call_selected_twice");
            return std::nullopt;
        }
        for (const auto id : call.cpu.owned_node_ids) {
            const auto & leaf = graph.event_for_node(id);
            if (!graph.node(id).active || graph.node(id).duration != leaf.dur) {
                issue("call_cost_already_changed");
                return std::nullopt;
            }
            if (id == *call.submission || leaf.ts + leaf.dur <= submit.ts) continue;
            if (leaf.ts < submit_end || leaf.ts + leaf.dur > finish || leaf.arg("hicache_control_semantics") != "parent_self_time") {
                issue("wait_contains_real_cpu_work");
                return std::nullopt;
            }
            for (const auto id_edge : source.incoming_edge_ids(id))
                if (graph.edge(id_edge).kind != Kind::Sequential) {
                    issue("self_has_external_dependency");
                    return std::nullopt;
                }
            for (const auto id_edge : source.outgoing_edge_ids(id))
                if (graph.edge(id_edge).kind != Kind::Sequential) {
                    issue("self_has_external_consumer");
                    return std::nullopt;
                }
            result.mutation.set_node_durations.push_back({ .node_id = id, .duration = 0, .effect_id = "cpu_collective" });
        }
        if (!graph.node(*call.worker).active || graph.node(*call.worker).duration != worker.dur || graph.node(*call.worker).cpu_ready_delay_before) {
            issue("worker_cost_already_changed");
            return std::nullopt;
        }
        auto * ingress = gap_before(*call.submission);
        if (!call.cpu.completion_anchor_node_id) {
            issue("missing_return_boundary");
            return std::nullopt;
        }
        const auto end_anchor = *call.cpu.completion_anchor_node_id;
        const auto & end_event = graph.event_for_node(end_anchor);
        // The ownership index may return the leaf ending exactly at the call
        // boundary, rather than the next leaf beginning after it.
        auto * egress = end_event.ts < finish && end_event.ts + end_event.dur == finish ? gap_after(end_anchor) : gap_before(end_anchor);
        if (!ingress || !egress || finish < egress->begin || finish > egress->end) {
            issue("return_not_in_gap");
            return std::nullopt;
        }
        const auto prefix = "cpu_collective:" + std::to_string(call.observation);
        const auto main_lane = std::string(graph.node_lane_key(*call.submission));
        const auto helper_lane = prefix + ":timing";
        const auto fork = marker(prefix + ":submit_start", main_lane);
        const auto dispatch = marker(prefix + ":dispatch", helper_lane, timing.dispatch);
        const auto arrival = marker(prefix + ":arrival", helper_lane);
        const auto joined = marker(prefix + ":join", helper_lane, timing.after_join);
        const auto returned = marker(prefix + ":return", main_lane);
        // These instants have observed coordinates even though their target
        // execution times change. Internal dispatch/join nodes have no such origin.
        result.mutation.synthetic_nodes[fork].node.observed_point =
            core::DagObservedPoint{ event.pid, event.tid, submit.ts, graph.node(*call.submission).gpu_id };
        result.mutation.synthetic_nodes[returned].node.observed_point =
            core::DagObservedPoint{ event.pid, event.tid, finish, graph.node(*call.submission).gpu_id };
        ingress->points.push_back({ submit.ts, fork, 1 });
        egress->points.push_back({ finish, returned, 0 });
        connect(ref(fork), ref(dispatch));
        connect(ref(dispatch), ref(arrival), Kind::Sequential);
        connect(Ref::existing(*call.submission), ref(joined));
        connect(Ref::existing(*call.worker), ref(joined));
        connect(ref(joined), ref(returned), Kind::Sequential);
        for (const auto id : source.incoming_edge_ids(*call.submission))
            if (graph.edge(id).kind != Kind::Sequential)
                result.mutation.redirect_edges.push_back({ .edge_index = id, .dst = ref(fork), .effect_id = "cpu_collective" });
        for (const auto & slice : call.cpu.owned_gap_slices) {
            const auto begin = std::max(slice.owned_start_us, submit_end), end = std::min(slice.owned_end_us, finish);
            if (end > begin)
                if (auto * gap = gap_after(slice.owner_node_id)) gap->cuts.push_back({ begin, end });
        }
        // Preserve worker FIFO edges, but not the observed wait for a future submission.
        for (const auto id : source.incoming_edge_ids(*call.worker)) {
            const auto & edge = graph.edge(id);
            if (edge.kind != Kind::Sequential) continue;
            const auto & before = graph.event_for_node(edge.src);
            if (graph.node(edge.src).lane_id != graph.node(*call.worker).lane_id || before.ts + before.dur > worker.ts
                || graph.node(edge.src).cpu_gap_after != worker.ts - before.ts - before.dur || graph.scope_gap_duration(edge.src) != 0) {
                issue("worker_queue_already_changed_or_overlapping");
                return std::nullopt;
            }
            if (std::ranges::count_if(source.outgoing_edge_ids(edge.src), [&](size_t out) { return graph.edge(out).kind == Kind::Sequential; }) != 1) {
                issue("worker_gap_has_multiple_consumers");
                return std::nullopt;
            }
            cpu_gaps[edge.src] = 0;
        }
        result.mutation.set_node_durations.push_back({ .node_id = *call.worker, .duration = timing.worker_remainder, .effect_id = "cpu_collective" });
        ++result.calls;
        return arrival;
    }
    void splice(size_t owner, Gap & gap) {
        std::ranges::sort(gap.points, {}, [](const auto & p) { return std::pair{ p.time, p.order }; });
        std::ranges::sort(gap.cuts);
        uint64_t previous = gap.begin;
        for (const auto & [begin, end] : gap.cuts) {
            if (begin < previous || end < begin || end > gap.end) {
                issue("overlapping_wait_ownership");
                return;
            }
            previous = end;
        }
        const auto retained = [&](uint64_t begin, uint64_t end) {
            uint64_t duration = end - begin;
            for (const auto & [a, b] : gap.cuts)
                if (std::max(a, begin) < std::min(b, end)) duration -= std::min(b, end) - std::max(a, begin);
            return duration;
        };
        cpu_gaps[owner] = retained(gap.begin, gap.points.empty() ? gap.end : gap.points.front().time);
        if (gap.points.empty()) return;
        result.mutation.disable_edges.push_back(gap.edge);
        auto last = Ref::existing(owner);
        for (size_t i = 0; i < gap.points.size(); ++i) {
            const auto & point = gap.points[i];
            connect(last, ref(point.node), Kind::Sequential);
            const auto end = i + 1 < gap.points.size() ? gap.points[i + 1].time : gap.end;
            result.mutation.synthetic_nodes[point.node].node.cpu_gap_after = retained(point.time, end);
            last = ref(point.node);
        }
        connect(last, Ref::existing(graph.edge(gap.edge).dst), Kind::Sequential);
    }
};
} // namespace

CpuCollectiveWaitPlan plan_cpu_collective_waits(const HiCacheSourceDagIndex & source, const std::vector<const CpuCollectiveRound *> & rounds) {
    return Planner(source).run(rounds);
}
} // namespace markov::trace_graph::modules::hicache::patch
