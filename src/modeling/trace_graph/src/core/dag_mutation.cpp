/**
 * @file
 * @brief Applies validated DAG mutation plans and records materialized actions.
 */
#include "markov/trace_graph/core/dag_mutation.hpp"

#include "dag_mutation_internal.hpp"

#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace markov::trace_graph::core {

namespace dag_mutation_detail {

std::string effective_value(std::string_view local, std::string_view fallback) { return std::string(local.empty() ? fallback : local); }

size_t resolve_applied_ref(const DagNodeRef & ref, const std::unordered_map<std::string, size_t> & synthetic_node_ids) {
    if (ref.existing_node_id) return *ref.existing_node_id;
    return synthetic_node_ids.at(ref.synthetic_id);
}

class DagMutationApplier {
public:
    DagMutationApplier(DagGraph & graph, const DagMutationPlan & plan) : graph_(graph), plan_(plan), nodes_disabled_by_plan_(graph.node_count(), false) {
        result_.journal.component = plan.component;
        result_.journal.active_nodes_before = graph.active_node_count();
        result_.journal.active_edges_before = graph.active_edge_count();
        active_effect_edges_.reserve(plan.redirect_edges.size() + plan.add_edges.size());
        for (const auto & edge : graph.edges()) {
            if (edge.active) increment_effect_edge(active_effect_edges_, edge.src, edge.dst, edge.kind, edge.effect_id());
        }
    }

    [[nodiscard]] DagMutationResult run() {
        add_synthetic_nodes();
        set_node_durations();
        set_node_e2e_eligibility();
        set_cpu_gaps();
        disable_explicit_edges();
        redirect_edges();
        disable_nodes_and_incident_edges();
        add_edges();
        result_.journal.active_nodes_after = graph_.active_node_count();
        result_.journal.active_edges_after = graph_.active_edge_count();
        return std::move(result_);
    }

private:
    void disable_tracked_edge(size_t edge_index) {
        const auto & edge = graph_.edge(edge_index);
        if (!edge.active) return;
        decrement_effect_edge(active_effect_edges_, edge.src, edge.dst, edge.kind, edge.effect_id());
        graph_.disable_edge(edge_index);
    }

    [[nodiscard]] std::optional<size_t> add_tracked_edge(size_t src, size_t dst, DagEdgeKind kind, const std::string & effect_id, const std::string & reason) {
        if (!effect_id.empty()) {
            const auto key = effect_edge_key(src, dst, kind, effect_id);
            if (active_effect_edges_.contains(key)) return std::nullopt;
        }
        else if (graph_.has_active_edge(src, dst, kind, effect_id)) return std::nullopt;

        const auto edge_index = graph_.add_edge(src, dst, kind, effect_id, reason);
        increment_effect_edge(active_effect_edges_, src, dst, kind, effect_id);
        return edge_index;
    }

    void add_synthetic_nodes() {
        for (const auto & mutation : plan_.synthetic_nodes) {
            auto spec = mutation.node;
            const auto effect_id = mutation.effect_id;
            const auto reason = effective_value(mutation.reason, plan_.reason);
            if (!effect_id.empty()) spec.attrs["effect_id"] = effect_id;
            if (!reason.empty()) spec.attrs["mutation_reason"] = reason;
            spec.attrs["synthetic_id"] = mutation.synthetic_id;
            const auto node_id = graph_.add_synthetic_node(spec);
            result_.synthetic_node_ids.emplace(mutation.synthetic_id, node_id);
            result_.journal.records.push_back(DagMutationRecord{
                .action = DagMutationAction::AddSyntheticNode,
                .effect_id = effect_id,
                .node_id = node_id,
                .edge_index = std::nullopt,
            });
        }
    }

    void set_node_durations() {
        for (const auto & mutation : plan_.set_node_durations) {
            const auto old_duration = graph_.node(mutation.node_id).duration;
            if (old_duration == mutation.duration) continue;
            graph_.set_node_duration(mutation.node_id, mutation.duration);
            result_.journal.records.push_back(DagMutationRecord{
                .action = DagMutationAction::SetNodeDuration,
                .effect_id = mutation.effect_id,
                .node_id = mutation.node_id,
                .new_duration = mutation.duration,
            });
        }
    }

    void set_node_e2e_eligibility() {
        for (const auto & mutation : plan_.set_node_e2e_eligibility) {
            const auto old_value = graph_.node(mutation.node_id).counts_toward_e2e;
            if (old_value == mutation.counts_toward_e2e) continue;
            graph_.set_node_counts_toward_e2e(mutation.node_id, mutation.counts_toward_e2e);
            result_.journal.records.push_back(DagMutationRecord{
                .action = DagMutationAction::SetNodeE2eEligibility,
                .effect_id = mutation.effect_id,
                .node_id = mutation.node_id,
                .new_counts_toward_e2e = mutation.counts_toward_e2e,
            });
        }
    }

    void set_cpu_gaps() {
        for (const auto & mutation : plan_.set_cpu_gaps) {
            const auto old_gap = graph_.node(mutation.node_id).cpu_gap_after;
            if (old_gap == mutation.duration) continue;
            graph_.set_cpu_gap_after(mutation.node_id, mutation.duration);
            result_.journal.records.push_back(DagMutationRecord{
                .action = DagMutationAction::SetCpuGap,
                .effect_id = mutation.effect_id,
                .node_id = mutation.node_id,
                .new_cpu_gap = mutation.duration,
            });
        }
    }

    void disable_explicit_edges() {
        for (const auto edge_index : plan_.disable_edges) {
            const auto was_active = graph_.edge(edge_index).active;
            disable_tracked_edge(edge_index);
            if (!was_active) continue;
            result_.journal.records.push_back(DagMutationRecord{
                .action = DagMutationAction::DisableEdge,
                .effect_id = {},
                .node_id = std::nullopt,
                .edge_index = edge_index,
            });
        }
    }

    void redirect_edges() {
        for (const auto & redirect : plan_.redirect_edges) {
            const auto original = graph_.edge(redirect.edge_index);
            disable_tracked_edge(redirect.edge_index);
            const auto src = redirect.src ? resolve_applied_ref(*redirect.src, result_.synthetic_node_ids) : original.src;
            const auto dst = redirect.dst ? resolve_applied_ref(*redirect.dst, result_.synthetic_node_ids) : original.dst;
            const auto effect_id = effective_value(redirect.effect_id, original.effect_id());
            const auto reason = effective_value(redirect.reason, plan_.reason);
            const auto new_edge_index = add_tracked_edge(src, dst, original.kind, effect_id, reason);
            result_.journal.records.push_back(DagMutationRecord{
                .action = DagMutationAction::RedirectEdge,
                .effect_id = effect_id,
                .node_id = std::nullopt,
                .edge_index = new_edge_index,
                .replaced_edge_index = redirect.edge_index,
                .src = src,
                .dst = dst,
            });
        }
    }

    void disable_nodes_and_incident_edges() {
        for (const auto node_id : plan_.disable_nodes) disable_node(node_id);
        if (plan_.disable_nodes.empty()) return;
        for (size_t edge_index = 0; edge_index < graph_.edge_count(); ++edge_index) disable_incident_edge(edge_index);
    }

    void disable_node(size_t node_id) {
        auto & node = graph_.mutable_node(node_id);
        if (!node.active) return;
        node.active = false;
        nodes_disabled_by_plan_[node_id] = true;
        result_.journal.records.push_back(DagMutationRecord{
            .action = DagMutationAction::DisableNode,
            .effect_id = {},
            .node_id = node_id,
            .edge_index = std::nullopt,
        });
    }

    void disable_incident_edge(size_t edge_index) {
        const auto & edge = graph_.edge(edge_index);
        if (!edge.active || (!nodes_disabled_by_plan_[edge.src] && !nodes_disabled_by_plan_[edge.dst])) return;
        disable_tracked_edge(edge_index);
        result_.journal.records.push_back(DagMutationRecord{
            .action = DagMutationAction::DisableEdge,
            .effect_id = {},
            .node_id = std::nullopt,
            .edge_index = edge_index,
        });
    }

    void add_edges() {
        for (const auto & addition : plan_.add_edges) {
            const auto src = resolve_applied_ref(addition.src, result_.synthetic_node_ids);
            const auto dst = resolve_applied_ref(addition.dst, result_.synthetic_node_ids);
            const auto effect_id = addition.effect_id;
            const auto reason = effective_value(addition.reason, plan_.reason);
            const auto edge_index = add_tracked_edge(src, dst, addition.kind, effect_id, reason);
            if (!edge_index) continue;
            result_.journal.records.push_back(DagMutationRecord{
                .action = DagMutationAction::AddEdge,
                .effect_id = effect_id,
                .node_id = std::nullopt,
                .edge_index = *edge_index,
                .src = src,
                .dst = dst,
            });
        }
    }

    DagGraph & graph_;
    const DagMutationPlan & plan_;
    DagMutationResult result_;
    std::vector<bool> nodes_disabled_by_plan_;
    EffectEdgeCounts active_effect_edges_;
};

DagMutationResult empty_mutation_result(const DagGraph & graph, const DagMutationPlan & plan) {
    DagMutationResult result;
    result.journal.component = plan.component;
    result.journal.active_nodes_before = graph.active_node_count();
    result.journal.active_edges_before = graph.active_edge_count();
    result.journal.active_nodes_after = result.journal.active_nodes_before;
    result.journal.active_edges_after = result.journal.active_edges_before;
    return result;
}

} // namespace dag_mutation_detail

using dag_mutation_detail::DagMutationApplier;
using dag_mutation_detail::empty_mutation_result;
using dag_mutation_detail::failure_message;

DagNodeRef DagNodeRef::existing(size_t node_id) { return DagNodeRef{ .existing_node_id = node_id, .synthetic_id = {} }; }

DagNodeRef DagNodeRef::synthetic(std::string synthetic_id) { return DagNodeRef{ .existing_node_id = std::nullopt, .synthetic_id = std::move(synthetic_id) }; }

bool DagMutationPlan::empty() const {
    return set_node_durations.empty() && set_node_e2e_eligibility.empty() && set_cpu_gaps.empty() && disable_nodes.empty() && disable_edges.empty()
           && synthetic_nodes.empty() && add_edges.empty() && redirect_edges.empty();
}

DagMutationValidationError::DagMutationValidationError(const std::string & message, DagTopologyValidationReport report)
    : std::runtime_error(message),
      report_(std::move(report)) {}


DagMutationResult apply_dag_mutation_plan(DagGraph & graph, const DagMutationPlan & plan) {
    if (plan.empty()) {
        auto result = empty_mutation_result(graph, plan);
#ifdef DEBUG
        result.topology = validate_active_dag(graph);
        if (!result.topology.ok())
            throw DagMutationValidationError("invalid active DAG before empty mutation plan: " + failure_message(result.topology), result.topology);
#endif
        return result;
    }
    const auto prospective = validate_dag_mutation_plan(graph, plan);
    if (!prospective.ok()) throw DagMutationValidationError("invalid DAG mutation plan: " + failure_message(prospective), prospective);

    auto result = DagMutationApplier(graph, plan).run();
#ifdef DEBUG
    result.topology = validate_active_dag(graph);
    if (!result.topology.ok())
        throw DagMutationValidationError("DAG mutation produced invalid active graph: " + failure_message(result.topology), result.topology);
#endif
    return result;
}

} // namespace markov::trace_graph::core
