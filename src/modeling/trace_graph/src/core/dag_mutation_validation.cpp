#include "markov/trace_graph/core/dag_mutation_internal.hpp"
#include "markov/trace_graph/core/dag_topology.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <ranges>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace markov::trace_graph::core {
namespace dag_mutation_detail {

constexpr size_t kInvalidIndex = std::numeric_limits<size_t>::max();

struct TopologyEdgeView {
    size_t src = 0, dst = 0;
    DagEdgeKind kind = DagEdgeKind::Mutation;
    bool active = true;
    std::string_view effect_id;
    size_t issue_edge_index = kInvalidIndex;
};

struct VirtualEdge {
    size_t src, dst;
    DagEdgeKind kind;
    std::string effect_id;
};

struct ProspectiveGraph {
    const DagGraph & base;
    std::vector<bool> active_nodes;
    std::vector<VirtualEdge> edges; // Only new edges, never another copy of the base graph.
    std::unordered_set<size_t> disabled_edges;
    std::unordered_map<std::string, size_t> synthetic_node_ids;
    std::vector<DagTopologyIssue> plan_issues;

    TopologyEdgeView edge_at(size_t id) const {
        if (id < base.edge_count()) {
            const auto & edge = base.edge(id);
            bool active = edge.active && !disabled_edges.contains(id);
            if (edge.src < active_nodes.size() && edge.dst < active_nodes.size()) active &= active_nodes[edge.src] && active_nodes[edge.dst];
            return { edge.src, edge.dst, edge.kind, active, edge.effect_id(), id };
        }
        const auto & edge = edges[id - base.edge_count()];
        return { edge.src, edge.dst, edge.kind, true, edge.effect_id, id };
    }
};

void add_issue(std::vector<DagTopologyIssue> & issues, std::string code, std::string message, std::vector<size_t> node_ids = {},
               std::vector<size_t> edge_indices = {}) {
    issues.push_back({ std::move(code), std::move(message), std::move(node_ids), std::move(edge_indices) });
}

std::optional<size_t> resolve_node_ref(const DagNodeRef & ref, const ProspectiveGraph & graph, std::vector<DagTopologyIssue> & issues) {
    const bool has_existing = ref.existing_node_id.has_value(), has_synthetic = !ref.synthetic_id.empty();
    if (has_existing == has_synthetic) {
        add_issue(issues, "invalid_node_reference", "node reference must select exactly one existing or synthetic node");
        return std::nullopt;
    }
    if (ref.existing_node_id) {
        if (*ref.existing_node_id >= graph.base.node_count()) {
            add_issue(issues, "invalid_node_reference", "existing node reference is out of range", { *ref.existing_node_id });
            return std::nullopt;
        }
        return ref.existing_node_id;
    }
    const auto found = graph.synthetic_node_ids.find(ref.synthetic_id);
    if (found == graph.synthetic_node_ids.end()) {
        add_issue(issues, "unknown_synthetic_node", "synthetic node reference is not declared: " + ref.synthetic_id);
        return std::nullopt;
    }
    return found->second;
}

class ProspectiveGraphBuilder {
public:
    ProspectiveGraphBuilder(const DagGraph & graph, const DagMutationPlan & plan) : graph_(graph), plan_(plan), prospective_{ graph } {}

    ProspectiveGraph run() {
        if (plan_.component.empty())
            add_issue(prospective_.plan_issues, "empty_component", "non-empty DAG mutation plan requires one semantic component owner");
        prospective_.active_nodes.reserve(graph_.node_count() + plan_.synthetic_nodes.size());
        for (const auto & node : graph_.nodes()) prospective_.active_nodes.push_back(node.active);
        prospective_.edges.reserve(plan_.add_edges.size() + plan_.redirect_edges.size());
        register_synthetic_nodes();
        validate_duration_updates();
        validate_e2e_eligibility_updates();
        validate_cpu_gap_updates();
        apply_node_disables();
        apply_edge_disables();
        apply_redirects();
        for (const auto & addition : plan_.add_edges) {
            const auto src = resolve_node_ref(addition.src, prospective_, prospective_.plan_issues);
            const auto dst = resolve_node_ref(addition.dst, prospective_, prospective_.plan_issues);
            if (src && dst) append_edge_if_unique({ *src, *dst, addition.kind, addition.effect_id });
        }
        return std::move(prospective_);
    }

private:
    void append_edge_if_unique(VirtualEdge edge) {
        if (!prospective_.active_nodes[edge.src] || !prospective_.active_nodes[edge.dst]) {
            add_issue(prospective_.plan_issues, "active_edge_to_disabled_node", "new dependency references a disabled node", { edge.src, edge.dst });
            return;
        }
        if (edge.src == edge.dst) {
            add_issue(prospective_.plan_issues, "active_self_loop", "new dependency creates a self-loop", { edge.src });
            return;
        }
        const auto key = effect_edge_key(edge.src, edge.dst, edge.kind, edge.effect_id);
        if (added_edges_.contains(key)) return;
        if (edge.src < graph_.node_count()) {
            const auto adjacency = graph_.adjacency();
            for (const auto id : adjacency->outgoing(edge.src)) {
                const auto & existing = graph_.edge(id);
                if (!prospective_.disabled_edges.contains(id) && existing.dst == edge.dst && existing.kind == edge.kind
                    && existing.effect_id() == edge.effect_id)
                    return;
            }
        }
        added_edges_.insert(key);
        prospective_.edges.push_back(std::move(edge));
    }

    void register_synthetic_nodes() {
        for (const auto & mutation : plan_.synthetic_nodes) {
            if (mutation.node.name.empty()) add_issue(prospective_.plan_issues, "empty_synthetic_name", "synthetic DAG node name must not be empty");
            if (mutation.node.cpu_task_ready_delay_us && !mutation.node.is_cpu)
                add_issue(prospective_.plan_issues, "non_cpu_task", "explicit CPU task must use a CPU node");
            if (mutation.node.cpu_gap_after && (!mutation.node.is_cpu || mutation.node.cpu_task_ready_delay_us))
                add_issue(prospective_.plan_issues, "invalid_synthetic_cpu_gap", "residual gap requires a non-worker CPU boundary");
            if (mutation.synthetic_id.empty()) {
                add_issue(prospective_.plan_issues, "empty_synthetic_id", "synthetic node id must not be empty");
                prospective_.active_nodes.push_back(true);
                continue;
            }
            const auto node_id = prospective_.active_nodes.size();
            if (!prospective_.synthetic_node_ids.emplace(mutation.synthetic_id, node_id).second) {
                add_issue(prospective_.plan_issues, "duplicate_synthetic_id", "synthetic node id is duplicated: " + mutation.synthetic_id);
            }
            prospective_.active_nodes.push_back(true);
        }
    }

    void validate_duration_updates() {
        std::unordered_set<size_t> seen;
        seen.reserve(plan_.set_node_durations.size());
        const std::unordered_set<size_t> disabled_nodes(plan_.disable_nodes.begin(), plan_.disable_nodes.end());
        for (const auto & mutation : plan_.set_node_durations) {
            if (!seen.insert(mutation.node_id).second) {
                add_issue(prospective_.plan_issues, "duplicate_node_duration_update", "node duration update is duplicated", { mutation.node_id });
                continue;
            }
            if (mutation.node_id >= graph_.node_count()) {
                add_issue(prospective_.plan_issues, "invalid_node_duration_update", "node duration update is out of range", { mutation.node_id });
                continue;
            }
            if (!graph_.node(mutation.node_id).active) {
                add_issue(prospective_.plan_issues, "inactive_node_duration_update", "node duration update targets an inactive node", { mutation.node_id });
            }
            if (disabled_nodes.contains(mutation.node_id)) {
                add_issue(prospective_.plan_issues,
                          "duration_disable_conflict",
                          "one node cannot receive a duration update and be disabled by the same plan",
                          { mutation.node_id });
            }
        }
    }

    void validate_e2e_eligibility_updates() {
        std::unordered_set<size_t> seen;
        seen.reserve(plan_.set_node_e2e_eligibility.size());
        const std::unordered_set<size_t> disabled_nodes(plan_.disable_nodes.begin(), plan_.disable_nodes.end());
        for (const auto & mutation : plan_.set_node_e2e_eligibility) {
            if (!seen.insert(mutation.node_id).second) {
                add_issue(prospective_.plan_issues, "duplicate_node_e2e_eligibility_update", "node E2E eligibility update is duplicated", { mutation.node_id });
                continue;
            }
            if (mutation.node_id >= graph_.node_count()) {
                add_issue(prospective_.plan_issues, "invalid_node_e2e_eligibility_update", "node E2E eligibility update is out of range", { mutation.node_id });
                continue;
            }
            if (!graph_.node(mutation.node_id).active) {
                add_issue(prospective_.plan_issues,
                          "inactive_node_e2e_eligibility_update",
                          "node E2E eligibility update targets an inactive node",
                          { mutation.node_id });
            }
            if (disabled_nodes.contains(mutation.node_id)) {
                add_issue(prospective_.plan_issues,
                          "e2e_eligibility_disable_conflict",
                          "one node cannot receive an E2E eligibility update and be disabled by the same plan",
                          { mutation.node_id });
            }
        }
    }

    void validate_cpu_gap_updates() {
        std::unordered_set<size_t> seen;
        seen.reserve(plan_.set_cpu_gaps.size());
        const std::unordered_set<size_t> disabled_nodes(plan_.disable_nodes.begin(), plan_.disable_nodes.end());
        for (const auto & mutation : plan_.set_cpu_gaps) {
            if (!seen.insert(mutation.node_id).second) {
                add_issue(prospective_.plan_issues, "duplicate_cpu_gap_update", "CPU gap update is duplicated", { mutation.node_id });
                continue;
            }
            if (mutation.node_id >= graph_.node_count()) {
                add_issue(prospective_.plan_issues, "invalid_cpu_gap_update", "CPU gap update is out of range", { mutation.node_id });
                continue;
            }
            const auto & node = graph_.node(mutation.node_id);
            if (!node.active) add_issue(prospective_.plan_issues, "inactive_cpu_gap_update", "CPU gap update targets an inactive node", { mutation.node_id });
            if (!node.is_cpu) add_issue(prospective_.plan_issues, "non_cpu_gap_update", "CPU gap update targets a non-CPU node", { mutation.node_id });
            if (mutation.retained_ranges) {
                try {
                    graph_.validate_cpu_gap_ranges(mutation.node_id, mutation.duration, *mutation.retained_ranges);
                }
                catch (const std::exception & error) {
                    add_issue(prospective_.plan_issues, "invalid_cpu_gap_ranges", error.what(), { mutation.node_id });
                }
            }
            if (disabled_nodes.contains(mutation.node_id)) {
                add_issue(prospective_.plan_issues,
                          "cpu_gap_disable_conflict",
                          "one node cannot receive a CPU gap update and be disabled by the same plan",
                          { mutation.node_id });
            }
        }
    }

    void apply_node_disables() {
        std::unordered_set<size_t> seen;
        seen.reserve(plan_.disable_nodes.size());
        for (const auto node_id : plan_.disable_nodes) {
            if (!seen.insert(node_id).second) {
                add_issue(prospective_.plan_issues, "duplicate_disable_node", "disabled node id is duplicated", { node_id });
                continue;
            }
            if (node_id >= graph_.node_count()) {
                add_issue(prospective_.plan_issues, "invalid_disable_node", "disabled node id is out of range", { node_id });
                continue;
            }
            prospective_.active_nodes[node_id] = false;
        }
    }

    void apply_edge_disables() {
        for (const auto id : plan_.disable_edges) {
            if (!disabled_edge_indices_.insert(id).second)
                add_issue(prospective_.plan_issues, "duplicate_disable_edge", "disabled edge index is duplicated", {}, { id });
            else if (id >= graph_.edge_count()) add_issue(prospective_.plan_issues, "invalid_disable_edge", "disabled edge index is out of range", {}, { id });
            else prospective_.disabled_edges.insert(id);
        }
    }

    void apply_redirects() {
        std::unordered_set<size_t> seen;
        for (const auto & redirect : plan_.redirect_edges) {
            const auto id = redirect.edge_index;
            if (id >= graph_.edge_count()) {
                add_issue(prospective_.plan_issues, "invalid_redirect_edge", "redirected edge index is out of range", {}, { id });
                continue;
            }
            if (!seen.insert(id).second) {
                add_issue(prospective_.plan_issues, "duplicate_redirect_edge", "redirected edge index is duplicated", {}, { id });
                continue;
            }
            if (disabled_edge_indices_.contains(id)) {
                add_issue(prospective_.plan_issues,
                          "disable_redirect_conflict",
                          "one edge cannot be both disabled and redirected by the same plan",
                          {},
                          { id });
                continue;
            }
            const auto & original = graph_.edge(id);
            if (!original.active) {
                add_issue(prospective_.plan_issues, "redirect_inactive_edge", "redirected edge is already inactive", {}, { id });
                continue;
            }
            prospective_.disabled_edges.insert(id);
            const auto src = redirect.src ? resolve_node_ref(*redirect.src, prospective_, prospective_.plan_issues) : std::optional<size_t>{ original.src };
            const auto dst = redirect.dst ? resolve_node_ref(*redirect.dst, prospective_, prospective_.plan_issues) : std::optional<size_t>{ original.dst };
            if (src && dst)
                append_edge_if_unique({ *src, *dst, original.kind, std::string(redirect.effect_id.empty() ? original.effect_id() : redirect.effect_id) });
        }
    }

    const DagGraph & graph_;
    const DagMutationPlan & plan_;
    ProspectiveGraph prospective_;
    std::unordered_set<EffectEdgeKey, EffectEdgeKeyHash> added_edges_;
    std::unordered_set<size_t> disabled_edge_indices_;
};

struct TopologyAdjacency {
    std::vector<size_t> offsets, destinations;
};
using MutationEdgeSet = std::unordered_set<EffectEdgeKey, EffectEdgeKeyHash>;

struct TopologyScan {
    DagTopologyValidationReport report;
    std::vector<size_t> outgoing_counts;
    std::vector<size_t> indegree;
    std::vector<uint8_t> valid_edges;
};

bool validate_active_edge_endpoints(const TopologyEdgeView & edge, const std::vector<bool> & active_nodes, std::vector<DagTopologyIssue> & issues) {
    if (edge.src >= active_nodes.size() || edge.dst >= active_nodes.size()) {
        add_issue(issues, "invalid_active_edge_endpoint", "active edge endpoint is out of range", { edge.src, edge.dst }, { edge.issue_edge_index });
        return false;
    }
    if (!active_nodes[edge.src] || !active_nodes[edge.dst]) {
        add_issue(issues, "active_edge_to_disabled_node", "active edge references a disabled node", { edge.src, edge.dst }, { edge.issue_edge_index });
        return false;
    }
    if (edge.src == edge.dst) {
        add_issue(issues, "active_self_loop", "active DAG contains a self-loop", { edge.src }, { edge.issue_edge_index });
        return false;
    }
    return true;
}

bool validate_mutation_edge_uniqueness(const TopologyEdgeView & edge, MutationEdgeSet & seen, std::vector<DagTopologyIssue> & issues) {
    if (edge.kind != DagEdgeKind::Mutation && edge.effect_id.empty()) return true;
    if (seen.insert(effect_edge_key(edge.src, edge.dst, edge.kind, edge.effect_id)).second) return true;
    add_issue(issues, "duplicate_mutation_edge", "active graph contains a duplicate mutation edge", { edge.src, edge.dst }, { edge.issue_edge_index });
    return false;
}

template <typename EdgeAccessor>
TopologyScan scan_topology_edges(const std::vector<bool> & active_nodes, size_t edge_count, std::vector<DagTopologyIssue> initial_issues,
                                 EdgeAccessor edge_at) {
    TopologyScan scan;
    scan.report.issues = std::move(initial_issues);
    scan.report.active_node_count = static_cast<size_t>(std::ranges::count(active_nodes, true));
    scan.outgoing_counts.resize(active_nodes.size(), 0);
    scan.indegree.resize(active_nodes.size(), 0);
    scan.valid_edges.resize(edge_count, 0);

    MutationEdgeSet mutation_edges;
    for (size_t edge_index = 0; edge_index < edge_count; ++edge_index) {
        const auto edge = edge_at(edge_index);
        if (!edge.active) continue;
        if (!validate_active_edge_endpoints(edge, active_nodes, scan.report.issues)) continue;
        if (!validate_mutation_edge_uniqueness(edge, mutation_edges, scan.report.issues)) continue;
        scan.report.active_edge_count++;
        scan.outgoing_counts[edge.src]++;
        scan.indegree[edge.dst]++;
        scan.valid_edges[edge_index] = 1;
    }
    return scan;
}

template <typename EdgeAccessor> TopologyAdjacency build_topology_adjacency(const TopologyScan & scan, size_t edge_count, EdgeAccessor edge_at) {
    TopologyAdjacency adjacency;
    adjacency.offsets.resize(scan.outgoing_counts.size() + 1, 0);
    for (size_t node_id = 0; node_id < scan.outgoing_counts.size(); ++node_id) {
        adjacency.offsets[node_id + 1] = adjacency.offsets[node_id] + scan.outgoing_counts[node_id];
    }
    adjacency.destinations.resize(scan.report.active_edge_count);
    auto cursor = adjacency.offsets;
    for (size_t edge_index = 0; edge_index < edge_count; ++edge_index) {
        if (scan.valid_edges[edge_index] == 0) continue;
        const auto edge = edge_at(edge_index);
        adjacency.destinations[cursor[edge.src]++] = edge.dst;
    }
    return adjacency;
}

std::vector<size_t> consume_acyclic_prefix(const std::vector<bool> & active_nodes, const TopologyAdjacency & adjacency, std::vector<size_t> & indegree) {
    std::vector<size_t> ready;
    ready.reserve(static_cast<size_t>(std::ranges::count(active_nodes, true)));
    for (size_t node_id = 0; node_id < active_nodes.size(); ++node_id) {
        if (active_nodes[node_id] && indegree[node_id] == 0) ready.push_back(node_id);
    }

    size_t ready_index = 0;
    while (ready_index < ready.size()) {
        const auto node_id = ready[ready_index++];
        for (size_t offset = adjacency.offsets[node_id]; offset < adjacency.offsets[node_id + 1]; ++offset) {
            const auto dst = adjacency.destinations[offset];
            indegree[dst]--;
            if (indegree[dst] == 0) ready.push_back(dst);
        }
    }
    return ready;
}

template <typename EdgeAccessor>
DagTopologyValidationReport validate_topology(const std::vector<bool> & active_nodes, size_t edge_count, std::vector<DagTopologyIssue> initial_issues,
                                              EdgeAccessor edge_at, std::shared_ptr<const DagTopologicalOrder> * order = nullptr) {
    auto scan = scan_topology_edges(active_nodes, edge_count, std::move(initial_issues), edge_at);
    const auto adjacency = build_topology_adjacency(scan, edge_count, edge_at);
    auto consumed = consume_acyclic_prefix(active_nodes, adjacency, scan.indegree);
    if (consumed.size() != scan.report.active_node_count) {
        scan.report.cycle_nodes = detail::find_unresolved_cycle(adjacency.offsets, adjacency.destinations, scan.indegree);
        add_issue(scan.report.issues, "active_dag_cycle", "active DAG contains a cycle", scan.report.cycle_nodes);
    }
    if (order && scan.report.ok()) {
        auto result = std::make_shared<DagTopologicalOrder>();
        result->nodes = std::move(consumed);
        for (size_t id = 0; id < active_nodes.size(); ++id)
            if (!active_nodes[id]) result->nodes.push_back(id);
        result->positions.resize(active_nodes.size());
        for (size_t at = 0; at < result->nodes.size(); ++at) result->positions[result->nodes[at]] = at;
        *order = std::move(result);
    }
    return std::move(scan.report);
}

// Keep the existing order if new work fits between its proven predecessors
// and successors. Otherwise the same overlay goes through full Kahn validation.
std::shared_ptr<const DagTopologicalOrder> extend_order(const ProspectiveGraph & graph, const DagTopologicalOrder & base) {
    const auto old_count = graph.base.node_count(), added = graph.active_nodes.size() - old_count;
    std::vector<size_t> slots(added, 0), indegree(added, 0);
    std::vector<std::vector<size_t>> successors(added);
    for (const auto & edge : graph.edges) {
        if (edge.src < old_count && edge.dst < old_count) {
            if (base.positions[edge.src] >= base.positions[edge.dst]) return {};
        }
        else if (edge.src < old_count) slots[edge.dst - old_count] = std::max(slots[edge.dst - old_count], base.positions[edge.src] + 1);
        else if (edge.dst >= old_count) {
            successors[edge.src - old_count].push_back(edge.dst - old_count);
            ++indegree[edge.dst - old_count];
        }
    }
    std::vector<size_t> ready;
    for (size_t id = 0; id < added; ++id)
        if (!indegree[id]) ready.push_back(id);
    for (size_t at = 0; at < ready.size(); ++at) {
        const auto src = ready[at];
        for (const auto dst : successors[src]) {
            slots[dst] = std::max(slots[dst], slots[src]);
            if (!--indegree[dst]) ready.push_back(dst);
        }
    }
    if (ready.size() != added) return {};
    for (const auto & edge : graph.edges)
        if (edge.src >= old_count && edge.dst < old_count && slots[edge.src - old_count] > base.positions[edge.dst]) return {};

    auto result = std::make_shared<DagTopologicalOrder>();
    result->nodes.reserve(graph.active_nodes.size());
    std::ranges::stable_sort(ready, {}, [&](size_t id) { return slots[id]; });
    size_t next = 0;
    for (size_t slot = 0; slot <= old_count; ++slot) {
        while (next < ready.size() && slots[ready[next]] == slot) result->nodes.push_back(old_count + ready[next++]);
        if (slot < old_count) result->nodes.push_back(base.nodes[slot]);
    }
    result->positions.resize(result->nodes.size());
    for (size_t at = 0; at < result->nodes.size(); ++at) result->positions[result->nodes[at]] = at;
    return result;
}

DagTopologyValidationReport validate_graph_storage(const DagGraph & graph, std::shared_ptr<const DagTopologicalOrder> * order) {
    std::vector<bool> active_nodes(graph.node_count(), false);
    for (const auto & node : graph.nodes()) active_nodes[node.id] = node.active;
    return validate_topology(
        active_nodes,
        graph.edge_count(),
        {},
        [&](size_t id) {
            const auto & edge = graph.edge(id);
            return TopologyEdgeView{ edge.src, edge.dst, edge.kind, edge.active, edge.effect_id(), id };
        },
        order);
}

PreparedDagMutation prepare_dag_mutation(const DagGraph & graph, const DagMutationPlan & plan) {
    auto prospective = ProspectiveGraphBuilder(graph, plan).run();
    if (!prospective.plan_issues.empty()) return { { .issues = std::move(prospective.plan_issues) }, {} };

    auto order = graph.topological_order();
    if (!order) {
        (void)validate_active_dag(graph);
        order = graph.topological_order();
    }
    if (order) {
        auto extended = prospective.active_nodes.size() == graph.node_count() ? order : extend_order(prospective, *order);
        if (prospective.active_nodes.size() == graph.node_count())
            for (const auto & edge : prospective.edges)
                if (order->positions[edge.src] >= order->positions[edge.dst]) {
                    extended.reset();
                    break;
                }
        if (extended) {
            auto removed = prospective.disabled_edges;
            if (!plan.disable_nodes.empty()) {
                const auto adjacency = graph.adjacency();
                for (const auto node : plan.disable_nodes) {
                    for (const auto edge : adjacency->incoming(node)) removed.insert(edge);
                    for (const auto edge : adjacency->outgoing(node)) removed.insert(edge);
                }
            }
            size_t removed_active = 0;
            for (const auto id : removed)
                if (graph.edge(id).active) ++removed_active;
            return {
                { .active_node_count = static_cast<size_t>(std::ranges::count(prospective.active_nodes, true)),
                 .active_edge_count = graph.active_edge_count() - removed_active + prospective.edges.size() },
                std::move(extended)
            };
        }
    }
    PreparedDagMutation result;
    result.topology = validate_topology(
        prospective.active_nodes,
        graph.edge_count() + prospective.edges.size(),
        {},
        [&](size_t id) { return prospective.edge_at(id); },
        &result.order);
    return result;
}

std::string failure_message(const DagTopologyValidationReport & report) {
    return report.issues.empty() ? "unknown topology validation failure" : report.issues.front().code + ": " + report.issues.front().message;
}

} // namespace dag_mutation_detail

DagTopologyValidationReport validate_active_dag(const DagGraph & graph) {
    std::shared_ptr<const DagTopologicalOrder> order;
    auto result = dag_mutation_detail::validate_graph_storage(graph, &order);
    graph.topological_order_ = std::move(order);
    return result;
}

DagTopologyValidationReport validate_dag_mutation_plan(const DagGraph & graph, const DagMutationPlan & plan) {
    return dag_mutation_detail::prepare_dag_mutation(graph, plan).topology;
}

} // namespace markov::trace_graph::core
