#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/modules/hicache/runtime/preparation.hpp"
#include <set>

namespace markov::trace_graph::modules::hicache::runtime {

std::map<size_t, size_t> bind_allocator_preparation_costs(core::DagGraph & graph, const AllocatorPreparationObservation & source) {
    using Ref = core::DagNodeRef;
    using Kind = core::DagEdgeKind;
    std::map<size_t, std::vector<size_t>> outgoing;
    for (const auto & call : source.calls)
        if (call.submit_gap_node) outgoing[*call.submit_gap_node];
    for (size_t e = 0; e < graph.edge_count(); ++e) {
        const auto & edge = graph.edge(e);
        if (edge.active && edge.kind == Kind::Sequential && outgoing.contains(edge.src)) outgoing.at(edge.src).push_back(e);
    }
    core::DagMutationPlan plan{ .component = "runtime_preparation", .reason = "reserve causal allocator preparation cost" };
    std::map<size_t, std::string> names;
    std::set<size_t> owners;
    for (size_t i = 0; i < source.calls.size(); ++i) {
        const auto & call = source.calls[i];
        if (!call.submit_gap_node) continue;
        const auto before = *call.submit_gap_node;
        const auto & node = graph.node(before);
        const auto & event = graph.event_for_node(before);
        const auto begin = core::checked_add_u64(event.ts, event.dur, "preparation gap start overflow");
        const auto end = core::checked_add_u64(begin, node.cpu_gap_after, "preparation gap end overflow");
        const auto & next = outgoing[before];
        if (!node.active || !node.is_cpu || node.cpu_gap_after != node.original_cpu_gap_after || graph.scope_gap_duration(before) || event.pid != call.pid
            || event.tid != call.tid || next.size() != 1 || !owners.insert(before).second || graph.event_for_node(graph.edge(next.front()).dst).ts != end
            || graph.node(graph.edge(next.front()).dst).lane_id != node.lane_id)
            throw std::runtime_error("Allocator preparation requires an unchanged, unique main-thread submission gap");
        uint64_t coverage = 0;
        for (const auto & [left, right] : call.intervals) {
            if (left < begin || right > end || right < left) throw std::runtime_error("Allocator preparation spans work outside its submission gap");
            coverage = core::checked_add_u64(coverage, right - left, "preparation coverage overflow");
        }
        if (coverage > node.cpu_gap_after) throw std::runtime_error("Allocator preparation intervals overlap");
        // The synthetic node has no source cost coordinates. Materialize the
        // corrected partition now, so moving this gap does not discard its
        // existing CPU correction or apply it again during simulation.
        const auto gap_service = graph.cpu_service_cost().duration({ call.pid, call.tid }, begin, end);
        const auto name = "allocator_preparation:" + std::to_string(i);
        names.emplace(i, name);
        plan.synthetic_nodes.push_back({
            .synthetic_id = name,
            .node = { .name = "allocator preparation",
                     .category = "runtime_preparation",
                     .is_cpu = true,
                     .lane_key = std::string(graph.node_lane_key(before)),
                     .duration = call.service_duration_us,
                     .cpu_gap_after = gap_service - call.service_duration_us,
                     .observed_point = core::DagObservedPoint{ call.pid, call.tid, begin, node.gpu_id } }
        });
        plan.set_cpu_gaps.push_back({ .node_id = before, .duration = 0 });
        plan.add_edges.push_back({ .src = Ref::existing(before), .dst = Ref::synthetic(name), .kind = Kind::Sequential });
        // Redirect only the sequential consumer. Independent consumers of
        // 'before' do not acquire the preparation cost.
        plan.redirect_edges.push_back({ .edge_index = next.front(), .src = Ref::synthetic(name) });
    }
    const auto applied = core::apply_dag_mutation_plan(graph, plan);
    std::map<size_t, size_t> result;
    for (const auto & [call, name] : names) result.emplace(call, applied.synthetic_node_ids.at(name));
    return result;
}

} // namespace markov::trace_graph::modules::hicache::runtime
