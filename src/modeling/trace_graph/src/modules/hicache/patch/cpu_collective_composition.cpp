#include "markov/trace_graph/modules/hicache/patch/cpu_collective_waits.hpp"
#include <algorithm>
#include <set>

namespace markov::trace_graph::modules::hicache::patch {
namespace {
// A dependency edit also claims its old/new endpoints. This deliberately keeps
// shared boundaries out until the two mechanisms can plan their timing jointly.
std::set<size_t> touched_nodes(const core::DagGraph & graph, const core::DagMutationPlan & plan) {
    std::set<size_t> nodes(plan.disable_nodes.begin(), plan.disable_nodes.end());
    const auto add_ref = [&](const core::DagNodeRef & ref) {
        if (ref.existing_node_id) nodes.insert(*ref.existing_node_id);
    };
    const auto add_edge = [&](size_t id) {
        nodes.insert(graph.edge(id).src);
        nodes.insert(graph.edge(id).dst);
    };
    for (const auto & update : plan.set_node_durations) nodes.insert(update.node_id);
    for (const auto & update : plan.set_cpu_gaps) nodes.insert(update.node_id);
    for (const auto & update : plan.set_node_e2e_eligibility) nodes.insert(update.node_id);
    for (const auto id : plan.disable_edges) add_edge(id);
    for (const auto & edge : plan.add_edges) {
        add_ref(edge.src);
        add_ref(edge.dst);
    }
    for (const auto & edge : plan.redirect_edges) {
        add_edge(edge.edge_index);
        if (edge.src) add_ref(*edge.src);
        if (edge.dst) add_ref(*edge.dst);
    }
    return nodes;
}
void append(core::DagMutationPlan & plan, const core::DagMutationPlan & addition) {
    const auto copy = [](auto & to, const auto & from) { to.insert(to.end(), from.begin(), from.end()); };
    copy(plan.set_node_durations, addition.set_node_durations);
    copy(plan.set_cpu_gaps, addition.set_cpu_gaps);
    copy(plan.set_node_e2e_eligibility, addition.set_node_e2e_eligibility);
    copy(plan.disable_nodes, addition.disable_nodes);
    copy(plan.disable_edges, addition.disable_edges);
    copy(plan.synthetic_nodes, addition.synthetic_nodes);
    copy(plan.add_edges, addition.add_edges);
    copy(plan.redirect_edges, addition.redirect_edges);
}
} // namespace

CpuCollectivePatch append_retained_cpu_collectives(const HiCacheSourceDagIndex & source, const CpuCollectiveObservation & observation,
                                                   core::DagMutationPlan & plan) {
    CpuCollectivePatch result;
    result.observed_rounds = observation.rounds.size();
    if (observation.rounds.empty()) return result;
    const auto & graph = source.graph();
    const auto occupied = touched_nodes(graph, plan);
    std::vector<const CpuCollectiveRound *> selected;
    for (const auto & round : observation.rounds) {
        if (!round.issue.empty() || std::ranges::any_of(round.calls, [](const auto & call) { return !call.issue.empty(); })) {
            ++result.source_retained_rounds["incomplete_observation"];
            continue;
        }
        const auto candidate = plan_cpu_collective_waits(source, { &round });
        if (!candidate.issues.empty()) {
            ++result.source_retained_rounds[candidate.issues.begin()->first];
            continue;
        }
        auto touched = touched_nodes(graph, candidate.mutation);
        // Some original leaves are read, not modified (notably submission CPU).
        // Their costs must still be the source values used by the timing model.
        for (const auto & call : round.calls) {
            touched.insert(call.cpu.owned_node_ids.begin(), call.cpu.owned_node_ids.end());
            touched.insert(*call.worker);
        }
        if (std::ranges::any_of(touched, [&](size_t id) { return occupied.contains(id); })) {
            ++result.source_retained_rounds["shared_transaction_boundary"];
            continue;
        }
        selected.push_back(&round);
    }
    if (selected.empty()) {
        result.status = "source_retained";
        return result;
    }
    // Generate once: adjacent calls can split different parts of the same gap.
    const auto combined = plan_cpu_collective_waits(source, selected);
    if (!combined.issues.empty()) {
        result.status = "blocked";
        result.blockers = combined.issues;
        return result;
    }
    append(plan, combined.mutation);
    result.planned_rounds = selected.size();
    result.status = result.source_retained_rounds.empty() ? "ready" : "partial";
    return result;
}
} // namespace markov::trace_graph::modules::hicache::patch
