#include "markov/trace_graph/modules/hicache/runtime/lifecycle_calls.hpp"
#include "markov/trace_graph/core/dag_mutation.hpp"
#include "markov/trace_graph/modules/hicache/execution_boundaries.hpp"
#include <algorithm>
#include <stdexcept>
#include <string>

namespace markov::trace_graph::modules::hicache::runtime {

void HiCacheLifecycleCalls::bind(core::DagGraph & graph, const model::HiCacheModelReplay & replay, uint64_t begin, uint64_t end, size_t opening_node) {
    std::vector<core::TraceEvent> points;
    std::vector<Action> actions;
    std::set<size_t> opening_owners;
    for (const auto & insert : observe_lifecycle_inserts(graph, replay.facts(), begin, end)) {
        if (!owners_.insert(insert.owner).second) throw std::runtime_error("Lifecycle has multiple insert calls");
        const auto & item = replay.fact(insert.owner);
        const auto append = [&](core::TraceEvent point, Kind kind, uint64_t tokens) {
            // These steps are initial state, not CPU work at the cut. The
            // thread may still be inside a native call, which cannot be split
            // merely to manufacture an entry for an already completed step.
            if (point.ts < begin) {
                actions_[opening_node].push_back({ kind, insert.owner, tokens });
                opening_owners.insert(insert.owner);
                return;
            }
            point.dur = point.dur_submicro_ns = 0;
            point.name = "hicache.lifecycle_step";
            points.push_back(std::move(point));
            actions.push_back({ kind, insert.owner, tokens });
        };
        append(insert.envelope, Kind::Entry, 0);
        for (const auto & step : lifecycle_ready_steps(insert)) append(step.ready, Kind::Ready, step.end_tokens);
        auto returned = insert.envelope;
        const auto ns = returned.ts_submicro_ns + returned.dur_submicro_ns;
        returned.ts += returned.dur + ns / 1'000;
        returned.ts_submicro_ns = ns % 1'000;
        append(returned, Kind::InsertReturn, item.fact.token_count);
        returned.ts = item.fact.source_ts;
        returned.ts_submicro_ns = 0;
        append(returned, Kind::Return, 0);
    }
    const auto nodes = bind_hicache_control_points(graph, points);
    core::DagMutationPlan opening{ .component = "hicache_lifecycle_opening" };
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (!nodes[i])
            throw std::runtime_error("Lifecycle step lacks an exact CPU boundary: pid=" + points[i].pid + " tid=" + points[i].tid
                                     + " source_us=" + std::to_string(points[i].ts) + " owner=" + std::to_string(actions[i].owner));
        actions_[*nodes[i]].push_back(actions[i]);
        if (opening_owners.contains(actions[i].owner) && *nodes[i] != opening_node)
            opening.add_edges.push_back({ core::DagNodeRef::existing(opening_node), core::DagNodeRef::existing(*nodes[i]), core::DagEdgeKind::Mutation });
    }
    if (!opening.add_edges.empty()) (void)core::apply_dag_mutation_plan(graph, opening);
}

std::vector<size_t> HiCacheLifecycleCalls::advance(size_t node, uint64_t time, model::HiCacheModelReplay & replay) const {
    std::vector<size_t> updated;
    const auto found = actions_.find(node);
    if (found == actions_.end()) return updated;
    for (const auto & action : found->second) {
        if (action.kind == Kind::Entry) replay.apply(action.owner, time, node, model::HiCacheLifecycleExecution::Stepped);
        else if (action.kind == Kind::Return) replay.return_lifecycle(action.owner, time, node);
        else replay.advance_lifecycle(action.owner, time, node, action.tokens);
        if (std::ranges::find(updated, action.owner) == updated.end()) updated.push_back(action.owner);
    }
    return updated;
}

} // namespace markov::trace_graph::modules::hicache::runtime
