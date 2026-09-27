#include "markov/trace_graph/modules/hicache/patch/prefetch_control_region.hpp"
#include "markov/trace_graph/core/cpu_gap_observation.hpp"
#include "markov/trace_graph/core/dag_mutation.hpp"
#include "markov/trace_graph/modules/hicache/model/prefetch_scheduler_body.hpp"
#include "markov/trace_graph/modules/hicache/patch/cpu_collective_waits.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <tuple>

namespace markov::trace_graph::modules::hicache::patch {
namespace {
struct SourceSpan {
    size_t fact;
    int rank;
    std::string pid, tid;
    uint64_t begin, end;
    std::vector<size_t> rounds;
};
SourceSpan span_for(const PrefetchControlObservation & c, const HiCacheSourceDagIndex & source) {
    if (!c.issue.empty() || !c.rank || c.cpu.empty()) throw std::invalid_argument("Control replacement requires fully observed calls");
    const auto * fact = source.fact_node(c.progress_fact);
    if (!fact) throw std::invalid_argument("Control replacement has no source fact");
    SourceSpan span{ c.progress_fact, *c.rank, fact->pid, fact->tid, c.cpu.front().cpu.interval_start_us, c.cpu.back().cpu.interval_end_us, {} };
    for (const auto round : { c.state_round, c.completion_round })
        if (round) span.rounds.push_back(*round);
    return span;
}
std::vector<PrefetchControlRegion> prepare_spans(core::DagGraph & graph, const std::vector<SourceSpan> & spans, const CpuCollectiveObservation & collectives) {
    if (spans.empty()) return {};
    std::vector<core::TraceEvent> points;
    std::map<size_t, size_t> worker_owner;
    std::map<size_t, std::set<int>> participants;
    for (size_t i = 0; i < spans.size(); ++i) {
        const auto & c = spans[i];
        for (const auto at : { c.begin, c.end }) {
            core::TraceEvent p;
            p.name = "hicache.control_boundary";
            p.pid = c.pid;
            p.tid = c.tid;
            p.ts = at;
            points.push_back(std::move(p));
        }
        for (const auto round_id : c.rounds) {
            const auto & round = collectives.rounds.at(round_id);
            if (!round.issue.empty()) throw std::invalid_argument("Control replacement has an incomplete collective");
            const auto call = std::ranges::find(round.calls, c.rank, &CpuCollectiveCall::rank);
            if (call == round.calls.end() || !call->issue.empty() || !call->worker || !participants[round_id].insert(c.rank).second
                || !worker_owner.emplace(*call->worker, i).second)
                throw std::invalid_argument("Control replacement has missing or duplicate communication ownership");
        }
    }
    for (const auto & [id, ranks] : participants) {
        const auto & round = collectives.rounds.at(id);
        if (ranks != std::set<int>(round.members.begin(), round.members.end()))
            throw std::invalid_argument("Control replacement must include the whole communication group");
    }
    auto bound = core::insert_cpu_gap_points(graph, points);
    // At an existing instantaneous CPU leaf, its two adjacent gaps can both
    // contain the timestamp. Reuse that unique unchanged leaf as a gate instead
    // of picking either gap by node number. Its outside dependencies stay intact.
    using Point = std::tuple<std::string, std::string, uint64_t>;
    std::map<Point, std::vector<size_t>> missing;
    for (size_t i = 0; i < points.size(); ++i)
        if (!bound[i]) missing[{ points[i].pid, points[i].tid, points[i].ts }].push_back(i);
    std::map<Point, std::vector<size_t>> existing;
    if (!missing.empty())
        for (const auto & node : graph.nodes()) {
            if (!node.active || !node.is_cpu || node.kind != core::DagNodeKind::TraceEvent || node.duration || node.cpu_gap_after != node.original_cpu_gap_after
                || graph.scope_gap_duration(node.id))
                continue;
            const auto & event = graph.event_for_node(node.id);
            const Point key{ event.pid, event.tid, event.ts };
            if (!event.dur && missing.contains(key)) existing[key].push_back(node.id);
        }
    for (const auto & [point, ids] : existing)
        if (ids.size() == 1)
            for (const auto i : missing.at(point)) bound[i] = ids.front();
    if (std::ranges::any_of(bound, [](const auto & p) { return !p; }))
        throw std::runtime_error("Control replacement lacks unique unchanged entry/exit boundaries");
    const HiCacheSourceDagIndex source(graph);
    core::DagMutationPlan plan{ .component = "hicache_control_execution", .reason = "replace observed control calls with target execution" };
    std::vector<PrefetchControlRegion> regions;
    std::map<size_t, size_t> main_owner;
    std::set<size_t> removed;
    for (size_t i = 0; i < spans.size(); ++i) {
        const auto entry = *bound[2 * i], exit = *bound[2 * i + 1];
        const auto &begin = points[2 * i], &end = points[2 * i + 1];
        const auto owned = source.timing_interval_ownership(begin.pid, begin.tid, begin.ts, end.ts - begin.ts);
        if (owned.status != "ready") throw std::runtime_error("Control replacement interval is not fully owned: " + owned.status + ": " + owned.reason);
        regions.push_back({ spans[i].fact, entry, exit, spans[i].rank });
        for (const auto id : owned.owned_node_ids) {
            if (!removed.insert(id).second || !main_owner.emplace(id, i).second) throw std::runtime_error("Control replacement intervals overlap");
        }
        // Timing ownership omits zero-duration leaves, but they still carry
        // the thread's dependency chain (e.g. a no-op aten::to). Follow the
        // actual entry-to-exit path rather than guessing boundary ties by time.
        auto current = entry;
        std::set<size_t> visited;
        while (current != exit) {
            if (!visited.insert(current).second) throw std::runtime_error("Control interval contains a sequential cycle");
            std::vector<size_t> next;
            for (const auto id : source.outgoing_edge_ids(current)) {
                const auto & edge = graph.edge(id);
                if (edge.kind == core::DagEdgeKind::Sequential && graph.node(edge.dst).lane_id == graph.node(entry).lane_id) next.push_back(edge.dst);
            }
            if (next.size() != 1) throw std::runtime_error("Control interval has no unique entry-to-exit CPU path");
            current = next.front();
            if (current == exit) break;
            if (removed.contains(current)) continue;
            const auto & node = graph.node(current);
            const auto & event = graph.event_for_node(current);
            if (node.kind != core::DagNodeKind::TraceEvent || node.duration || event.dur || event.ts < begin.ts || event.ts > end.ts)
                throw std::runtime_error("Control CPU path contains work outside its observed ownership");
            removed.insert(current);
            main_owner.emplace(current, i);
        }
        for (const auto & gap : owned.owned_gap_slices)
            if (gap.owner_node_id != entry && !removed.contains(gap.owner_node_id))
                throw std::runtime_error("Control replacement would consume an external CPU gap");
        plan.set_cpu_gaps.push_back({ .node_id = entry, .duration = 0 });
        plan.add_edges.push_back({ .src = core::DagNodeRef::existing(entry), .dst = core::DagNodeRef::existing(exit), .kind = core::DagEdgeKind::Sequential });
    }
    for (const auto [id, owner] : worker_owner) removed.insert(id);
    // Removing one Gloo task must not erase independent device/CPU consumers.
    // Ordinary worker FIFO edges are bypassed below; all other external edges
    // must meet the original main-thread entry/exit for that call.
    for (const auto & edge : graph.edges()) {
        if (!edge.active || removed.contains(edge.src) == removed.contains(edge.dst)) continue;
        const bool outgoing = removed.contains(edge.src);
        const auto id = outgoing ? edge.src : edge.dst, peer = outgoing ? edge.dst : edge.src;
        if (const auto at = main_owner.find(id); at != main_owner.end()) {
            const auto & r = regions.at(at->second);
            if (edge.kind != core::DagEdgeKind::Sequential || peer != (outgoing ? r.exit : r.entry))
                throw std::runtime_error("Control replacement has an unexplained external main-thread dependency: " + std::to_string(edge.src) + " "
                                         + graph.event_for_node(edge.src).name + " -> " + std::to_string(edge.dst) + " " + graph.event_for_node(edge.dst).name
                                         + " kind=" + std::to_string(static_cast<int>(edge.kind)));
        }
        else if (edge.kind != core::DagEdgeKind::Sequential || graph.node(peer).lane_id != graph.node(id).lane_id)
            throw std::runtime_error("Control replacement has an unexplained external worker dependency");
    }
    std::map<size_t, size_t> worker_round;
    for (size_t i = 0; i < collectives.rounds.size(); ++i)
        for (const auto & call : collectives.rounds[i].calls)
            if (call.worker) worker_round[*call.worker] = i;
    std::set<size_t> successors;
    for (const auto [id, owner] : worker_owner) {
        // Only heads of removed FIFO runs need a bypass. Preserve ordinary
        // tasks on either side, without carrying removed service into a gap.
        std::vector<size_t> before, after;
        bool internal_predecessor = false;
        for (const auto e : source.incoming_edge_ids(id)) {
            const auto & edge = graph.edge(e);
            if (edge.kind != core::DagEdgeKind::Sequential) continue;
            if (worker_owner.contains(edge.src)) internal_predecessor = true;
            else before.push_back(edge.src);
        }
        if (internal_predecessor) continue;
        size_t last = id;
        std::set<size_t> visited;
        while (visited.insert(last).second) {
            after.clear();
            for (const auto e : source.outgoing_edge_ids(last))
                if (graph.edge(e).kind == core::DagEdgeKind::Sequential) after.push_back(graph.edge(e).dst);
            if (after.size() != 1 || !worker_owner.contains(after.front())) break;
            last = after.front();
        }
        if (before.size() > 1 || after.size() > 1) throw std::runtime_error("Control worker FIFO is not a single lane");
        if (after.empty()) continue;
        const auto next = worker_round.find(after.front());
        if (next == worker_round.end()) throw std::runtime_error("Control worker successor has no observed submission timing");
        const auto & calls = collectives.rounds[next->second].calls;
        const auto call = std::ranges::find(calls, after.front(), [](const auto & c) { return c.worker.value_or(core::DagNode::kNoNode); });
        const auto & event = graph.runtime_observations().at(call->observation);
        if (event.pid != points[2 * owner].pid || event.tid != points[2 * owner].tid)
            throw std::runtime_error("Control worker is shared by independent callers");
        successors.insert(next->second);
        if (!before.empty()) {
            plan.set_cpu_gaps.push_back({ .node_id = before.front(), .duration = 0 });
            plan.add_edges.push_back(
                { .src = core::DagNodeRef::existing(before.front()), .dst = core::DagNodeRef::existing(after.front()), .kind = core::DagEdgeKind::Sequential });
        }
    }
    // The successor's old idle gap referred to removed work. Reuse the native
    // timing planner to make it wait for its own submission/peers instead.
    std::vector<const CpuCollectiveRound *> next_rounds;
    for (const auto id : successors) next_rounds.push_back(&collectives.rounds[id]);
    if (!next_rounds.empty()) {
        auto next = plan_cpu_collective_waits(source, next_rounds);
        if (!next.issues.empty()) throw std::runtime_error("Control worker successor cannot be retimed: " + next.issues.begin()->first);
        for (const auto & gap : plan.set_cpu_gaps) {
            const auto old = std::ranges::find(next.mutation.set_cpu_gaps, gap.node_id, &core::DagSetCpuGapMutation::node_id);
            if (old == next.mutation.set_cpu_gaps.end()) next.mutation.set_cpu_gaps.push_back(gap);
            else if (old->duration != gap.duration) throw std::runtime_error("Control and successor claim different residual gaps");
        }
        next.mutation.add_edges.insert(next.mutation.add_edges.end(), plan.add_edges.begin(), plan.add_edges.end());
        next.mutation.component = plan.component;
        plan = std::move(next.mutation);
        std::erase_if(plan.set_cpu_gaps, [&](const auto & gap) { return removed.contains(gap.node_id); });
    }
    plan.disable_nodes.assign(removed.begin(), removed.end());
    (void)core::apply_dag_mutation_plan(graph, plan);
    return regions;
}
} // namespace

std::vector<PrefetchControlRegion> prepare_prefetch_control_regions(core::DagGraph & graph, const std::vector<PrefetchControlObservation> & controls,
                                                                    const CpuCollectiveObservation & collectives) {
    const HiCacheSourceDagIndex source(graph);
    std::vector<SourceSpan> spans;
    for (const auto & c : controls) spans.push_back(span_for(c, source));
    return prepare_spans(graph, spans, collectives);
}

std::vector<PrefetchControlRegion> prepare_prefetch_wait_regions(core::DagGraph & graph, const std::vector<PrefetchControlObservation> & controls,
                                                                 const CpuCollectiveObservation & collectives, const PrefetchWorkerObservations & workers) {
    if (controls.empty()) return {};
    const HiCacheSourceDagIndex source(graph);
    std::map<std::string, std::map<int, std::vector<PrefetchControlObservation>>> requests;
    for (const auto & c : controls) {
        (void)span_for(c, source);
        const auto * fact = source.fact_node(c.progress_fact);
        requests[fact->request_id][*c.rank].push_back(c);
    }
    std::vector<SourceSpan> spans;
    for (auto & [request, ranks] : requests) {
        const auto count = ranks.begin()->second.size();
        const auto first_span = spans.size();
        for (auto & [rank, calls] : ranks) {
            std::ranges::sort(calls, {}, [](const auto & c) { return c.cpu.front().cpu.interval_start_us; });
            if (calls.size() != count || source.fact_node(calls.back().progress_fact)->progress_ready != true)
                throw std::invalid_argument("A wait region requires all ranks and their final successful return");
            auto span = span_for(calls.front(), source);
            span.rounds.clear();
            span.end = calls.back().cpu.back().cpu.interval_end_us;
            for (size_t i = 0; i < calls.size(); ++i) {
                const auto local = span_for(calls[i], source);
                if (local.pid != span.pid || local.tid != span.tid || (i + 1 < calls.size() && source.fact_node(local.fact)->progress_ready != false))
                    throw std::invalid_argument("A wait region must be one contiguous scheduler wait");
                span.rounds.insert(span.rounds.end(), local.rounds.begin(), local.rounds.end());
            }
            spans.push_back(std::move(span));
        }
        for (size_t i = 1; i < count; ++i) {
            std::vector<PrefetchControlObservation> previous, next;
            for (const auto & [rank, calls] : ranks) {
                previous.push_back(calls[i - 1]);
                next.push_back(calls[i]);
            }
            const auto body = model::observe_prefetch_scheduler_body(source, collectives, workers, previous, next);
            if (!body.issue.empty()) throw std::invalid_argument("Incomplete source wait body: " + body.issue);
            for (size_t j = first_span; j < spans.size(); ++j)
                for (const auto & step : body.steps) spans[j].rounds.push_back(step.source_round);
        }
    }
    return prepare_spans(graph, spans, collectives);
}
} // namespace markov::trace_graph::modules::hicache::patch
