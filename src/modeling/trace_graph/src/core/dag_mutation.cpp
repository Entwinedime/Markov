/**
 * @file
 * @brief Prospective DAG mutation validation, application, and journaling.
 */
#include "markov/trace_graph/core/dag_mutation.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <queue>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace markov::trace_graph::core {

namespace dag_mutation_detail {

constexpr size_t kInvalidIndex = std::numeric_limits<size_t>::max();

struct VirtualEdge {
    size_t src = 0;
    size_t dst = 0;
    DagEdgeKind kind = DagEdgeKind::Mutation;
    bool active = true;
    bool added_after_node_disable = false;
    std::string effect_id;
    size_t source_edge_index = kInvalidIndex;
};

struct DfsFrame {
    size_t node_id = kInvalidIndex;
    size_t next_edge_index = 0;
};

struct ProspectiveGraph {
    std::vector<bool> active_nodes;
    std::vector<VirtualEdge> edges;
    std::unordered_map<std::string, size_t> synthetic_node_ids;
    std::vector<DagTopologyIssue> plan_issues;
};

/** @brief Identity of one provenance-bearing edge tracked during a mutation. */
struct EffectEdgeKey {
    size_t src = 0;
    size_t dst = 0;
    DagEdgeKind kind = DagEdgeKind::Mutation;
    std::string effect_id;

    bool operator==(const EffectEdgeKey &) const = default;
};

/** @brief Hashes the narrow edge set that can collide with model-created edges. */
struct EffectEdgeKeyHash {
    size_t operator()(const EffectEdgeKey & key) const noexcept {
        auto combine = [](size_t seed, size_t value) { return seed ^ (value + static_cast<size_t>(0x9e37'79b9'7f4a'7c15ULL) + (seed << 6U) + (seed >> 2U)); };
        size_t value = std::hash<size_t>{}(key.src);
        value = combine(value, std::hash<size_t>{}(key.dst));
        value = combine(value, std::hash<unsigned int>{}(static_cast<unsigned int>(key.kind)));
        return combine(value, std::hash<std::string>{}(key.effect_id));
    }
};

using EffectEdgeCounts = std::unordered_map<EffectEdgeKey, size_t, EffectEdgeKeyHash>;

EffectEdgeKey effect_edge_key(size_t src, size_t dst, DagEdgeKind kind, std::string_view effect_id) {
    return EffectEdgeKey{
        .src = src,
        .dst = dst,
        .kind = kind,
        .effect_id = std::string(effect_id),
    };
}

void increment_effect_edge(EffectEdgeCounts & counts, size_t src, size_t dst, DagEdgeKind kind, std::string_view effect_id) {
    if (effect_id.empty()) return;
    ++counts[effect_edge_key(src, dst, kind, effect_id)];
}

void decrement_effect_edge(EffectEdgeCounts & counts, size_t src, size_t dst, DagEdgeKind kind, std::string_view effect_id) {
    if (effect_id.empty()) return;
    const auto key = effect_edge_key(src, dst, kind, effect_id);
    const auto found = counts.find(key);
    if (found == counts.end()) return;
    if (found->second <= 1) counts.erase(found);
    else --found->second;
}

void add_issue(std::vector<DagTopologyIssue> & issues, std::string code, std::string message, std::vector<size_t> node_ids = {},
               std::vector<size_t> edge_indices = {}) {
    issues.push_back(DagTopologyIssue{
        .code = std::move(code),
        .message = std::move(message),
        .node_ids = std::move(node_ids),
        .edge_indices = std::move(edge_indices),
    });
}

std::optional<size_t> resolve_node_ref(const DagNodeRef & ref, const ProspectiveGraph & graph, std::vector<DagTopologyIssue> & issues) {
    const bool has_existing = ref.existing_node_id.has_value();
    const bool has_synthetic = !ref.synthetic_id.empty();
    if (has_existing == has_synthetic) {
        add_issue(issues, "invalid_node_reference", "node reference must select exactly one existing or synthetic node");
        return std::nullopt;
    }
    if (has_existing) {
        if (*ref.existing_node_id >= graph.active_nodes.size()) {
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

bool same_edge(const VirtualEdge & lhs, const VirtualEdge & rhs) {
    return lhs.active && rhs.active && lhs.src == rhs.src && lhs.dst == rhs.dst && lhs.kind == rhs.kind && lhs.effect_id == rhs.effect_id;
}

class ProspectiveGraphBuilder {
public:
    ProspectiveGraphBuilder(const DagGraph & graph, const DagMutationPlan & plan) : graph_(graph), plan_(plan) {
        disabled_edge_indices_.reserve(plan.disable_edges.size());
    }

    [[nodiscard]] ProspectiveGraph run() {
        copy_active_graph();
        register_synthetic_nodes();
        validate_duration_updates();
        apply_node_disables();
        apply_edge_disables();
        apply_redirects();
        apply_additions();
        deactivate_original_edges_incident_to_disabled_nodes();
        return std::move(prospective_);
    }

private:
    void copy_active_graph() {
        if (plan_.plan_id.empty()) add_issue(prospective_.plan_issues, "empty_plan_id", "non-empty DAG mutation plan requires a plan id");
        active_effect_edges_.reserve(plan_.redirect_edges.size() + plan_.add_edges.size());
        prospective_.active_nodes.reserve(graph_.node_count() + plan_.synthetic_nodes.size());
        for (const auto & node : graph_.nodes()) prospective_.active_nodes.push_back(node.active);
        prospective_.edges.reserve(graph_.edge_count() + plan_.add_edges.size() + plan_.redirect_edges.size());
        for (size_t edge_index = 0; edge_index < graph_.edge_count(); ++edge_index) {
            const auto & edge = graph_.edge(edge_index);
            prospective_.edges.push_back(VirtualEdge{
                .src = edge.src,
                .dst = edge.dst,
                .kind = edge.kind,
                .active = edge.active,
                .effect_id = std::string(edge.effect_id()),
                .source_edge_index = edge_index,
            });
            if (edge.active) increment_effect_edge(active_effect_edges_, edge.src, edge.dst, edge.kind, edge.effect_id());
        }
    }

    void deactivate_edge(size_t edge_index) {
        auto & edge = prospective_.edges[edge_index];
        if (!edge.active) return;
        decrement_effect_edge(active_effect_edges_, edge.src, edge.dst, edge.kind, edge.effect_id);
        edge.active = false;
    }

    void append_edge_if_unique(VirtualEdge edge) {
        if (!edge.effect_id.empty()) {
            const auto key = effect_edge_key(edge.src, edge.dst, edge.kind, edge.effect_id);
            if (active_effect_edges_.contains(key)) return;
            increment_effect_edge(active_effect_edges_, edge.src, edge.dst, edge.kind, edge.effect_id);
            prospective_.edges.push_back(std::move(edge));
            return;
        }
        if (std::ranges::any_of(prospective_.edges, [&](const auto & existing) { return same_edge(existing, edge); })) return;
        prospective_.edges.push_back(std::move(edge));
    }

    void register_synthetic_nodes() {
        for (const auto & mutation : plan_.synthetic_nodes) {
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
        for (const auto edge_index : plan_.disable_edges) {
            if (!disabled_edge_indices_.insert(edge_index).second) {
                add_issue(prospective_.plan_issues, "duplicate_disable_edge", "disabled edge index is duplicated", {}, { edge_index });
                continue;
            }
            if (edge_index >= prospective_.edges.size()) {
                add_issue(prospective_.plan_issues, "invalid_disable_edge", "disabled edge index is out of range", {}, { edge_index });
                continue;
            }
            deactivate_edge(edge_index);
        }
    }

    [[nodiscard]] bool validate_redirect_identity(const DagRedirectEdgeMutation & redirect, std::unordered_set<size_t> & seen) {
        if (redirect.edge_index >= graph_.edge_count()) {
            add_issue(prospective_.plan_issues, "invalid_redirect_edge", "redirected edge index is out of range", {}, { redirect.edge_index });
            return false;
        }
        if (!seen.insert(redirect.edge_index).second) {
            add_issue(prospective_.plan_issues, "duplicate_redirect_edge", "redirected edge index is duplicated", {}, { redirect.edge_index });
            return false;
        }
        if (disabled_edge_indices_.contains(redirect.edge_index)) {
            add_issue(prospective_.plan_issues,
                      "disable_redirect_conflict",
                      "one edge cannot be both disabled and redirected by the same plan",
                      {},
                      { redirect.edge_index });
            return false;
        }
        return true;
    }

    [[nodiscard]] std::string redirect_effect_id(const DagRedirectEdgeMutation & redirect, const VirtualEdge & original) const {
        if (!redirect.effect_id.empty()) return redirect.effect_id;
        return original.effect_id.empty() ? plan_.effect_id : original.effect_id;
    }

    void apply_redirects() {
        std::unordered_set<size_t> seen;
        seen.reserve(plan_.redirect_edges.size());
        for (const auto & redirect : plan_.redirect_edges) {
            if (!validate_redirect_identity(redirect, seen)) continue;
            const auto original = prospective_.edges[redirect.edge_index];
            if (!original.active) {
                add_issue(prospective_.plan_issues, "redirect_inactive_edge", "redirected edge is already inactive", {}, { redirect.edge_index });
                continue;
            }
            deactivate_edge(redirect.edge_index);
            const auto src = redirect.src ? resolve_node_ref(*redirect.src, prospective_, prospective_.plan_issues) : std::optional<size_t>{ original.src };
            const auto dst = redirect.dst ? resolve_node_ref(*redirect.dst, prospective_, prospective_.plan_issues) : std::optional<size_t>{ original.dst };
            if (!src || !dst) continue;
            append_edge_if_unique(VirtualEdge{
                .src = *src,
                .dst = *dst,
                .kind = original.kind,
                .added_after_node_disable = true,
                .effect_id = redirect_effect_id(redirect, original),
            });
        }
    }

    void apply_additions() {
        for (const auto & addition : plan_.add_edges) {
            const auto src = resolve_node_ref(addition.src, prospective_, prospective_.plan_issues);
            const auto dst = resolve_node_ref(addition.dst, prospective_, prospective_.plan_issues);
            if (!src || !dst) continue;
            append_edge_if_unique(VirtualEdge{
                .src = *src,
                .dst = *dst,
                .kind = addition.kind,
                .added_after_node_disable = true,
                .effect_id = addition.effect_id.empty() ? plan_.effect_id : addition.effect_id,
            });
        }
    }

    void deactivate_original_edges_incident_to_disabled_nodes() {
        for (size_t edge_index = 0; edge_index < prospective_.edges.size(); ++edge_index) {
            const auto & edge = prospective_.edges[edge_index];
            if (!edge.active || edge.added_after_node_disable) continue;
            if (edge.src >= prospective_.active_nodes.size() || edge.dst >= prospective_.active_nodes.size()) continue;
            if (!prospective_.active_nodes[edge.src] || !prospective_.active_nodes[edge.dst]) deactivate_edge(edge_index);
        }
    }

    const DagGraph & graph_;
    const DagMutationPlan & plan_;
    ProspectiveGraph prospective_;
    EffectEdgeCounts active_effect_edges_;
    std::unordered_set<size_t> disabled_edge_indices_;
};

ProspectiveGraph build_prospective_graph(const DagGraph & graph, const DagMutationPlan & plan) { return ProspectiveGraphBuilder(graph, plan).run(); }

struct TopologyAdjacency {
    std::vector<size_t> offsets;
    std::vector<size_t> destinations;
};

std::vector<size_t> find_cycle_nodes(const TopologyAdjacency & adjacency, const std::vector<bool> & active_nodes,
                                     const std::vector<size_t> & remaining_indegree) {
    std::vector<int> state(active_nodes.size(), 0);
    std::vector<size_t> path;
    std::vector<size_t> position(active_nodes.size(), kInvalidIndex);
    std::vector<DfsFrame> dfs;

    for (size_t start = 0; start < active_nodes.size(); ++start) {
        if (!active_nodes[start] || remaining_indegree[start] <= 0 || state[start] != 0) continue;
        path.clear();
        dfs.clear();
        state[start] = 1;
        position[start] = 0;
        path.push_back(start);
        dfs.push_back(DfsFrame{ .node_id = start, .next_edge_index = adjacency.offsets[start] });

        while (!dfs.empty()) {
            auto & frame = dfs.back();
            bool descended = false;
            while (frame.next_edge_index < adjacency.offsets[frame.node_id + 1]) {
                const auto dst = adjacency.destinations[frame.next_edge_index++];
                if (!active_nodes[dst] || remaining_indegree[dst] <= 0) continue;
                if (state[dst] == 0) {
                    state[dst] = 1;
                    position[dst] = path.size();
                    path.push_back(dst);
                    dfs.push_back(DfsFrame{ .node_id = dst, .next_edge_index = adjacency.offsets[dst] });
                    descended = true;
                    break;
                }
                if (state[dst] == 1) return std::vector<size_t>(path.begin() + static_cast<std::ptrdiff_t>(position[dst]), path.end());
            }
            if (descended) continue;
            const auto done = frame.node_id;
            dfs.pop_back();
            state[done] = 2;
            position[done] = kInvalidIndex;
            path.pop_back();
        }
    }
    return {};
}

struct TopologyEdgeView {
    size_t src = 0;
    size_t dst = 0;
    DagEdgeKind kind = DagEdgeKind::Mutation;
    bool active = true;
    std::string_view effect_id;
    size_t issue_edge_index = kInvalidIndex;
};

using MutationEdgeKey = std::tuple<size_t, size_t, DagEdgeKind, std::string_view>;

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

bool validate_mutation_edge_uniqueness(const TopologyEdgeView & edge, std::set<MutationEdgeKey> & seen, std::vector<DagTopologyIssue> & issues) {
    if (edge.kind != DagEdgeKind::Mutation && edge.effect_id.empty()) return true;
    const auto key = std::make_tuple(edge.src, edge.dst, edge.kind, edge.effect_id);
    if (seen.insert(key).second) return true;
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

    std::set<MutationEdgeKey> mutation_edges;
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

size_t consume_acyclic_prefix(const std::vector<bool> & active_nodes, const TopologyAdjacency & adjacency, std::vector<size_t> & indegree) {
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
    return ready.size();
}

template <typename EdgeAccessor>
DagTopologyValidationReport validate_topology(const std::vector<bool> & active_nodes, size_t edge_count, std::vector<DagTopologyIssue> initial_issues,
                                              EdgeAccessor edge_at) {
    auto scan = scan_topology_edges(active_nodes, edge_count, std::move(initial_issues), edge_at);
    const auto adjacency = build_topology_adjacency(scan, edge_count, edge_at);
    if (consume_acyclic_prefix(active_nodes, adjacency, scan.indegree) != scan.report.active_node_count) {
        scan.report.cycle_nodes = find_cycle_nodes(adjacency, active_nodes, scan.indegree);
        add_issue(scan.report.issues, "active_dag_cycle", "active DAG contains a cycle", scan.report.cycle_nodes);
    }
    return std::move(scan.report);
}

DagTopologyValidationReport validate_graph_storage(const DagGraph & graph) {
    std::vector<bool> active_nodes(graph.node_count(), false);
    for (const auto & node : graph.nodes()) active_nodes[node.id] = node.active;
    return validate_topology(active_nodes, graph.edge_count(), {}, [&](size_t edge_index) {
        const auto & edge = graph.edge(edge_index);
        return TopologyEdgeView{
            .src = edge.src,
            .dst = edge.dst,
            .kind = edge.kind,
            .active = edge.active,
            .effect_id = edge.effect_id(),
            .issue_edge_index = edge_index,
        };
    });
}

DagTopologyValidationReport validate_prospective_graph(const ProspectiveGraph & graph) {
    return validate_topology(graph.active_nodes, graph.edges.size(), graph.plan_issues, [&](size_t edge_index) {
        const auto & edge = graph.edges[edge_index];
        return TopologyEdgeView{
            .src = edge.src,
            .dst = edge.dst,
            .kind = edge.kind,
            .active = edge.active,
            .effect_id = edge.effect_id,
            .issue_edge_index = edge.source_edge_index == kInvalidIndex ? edge_index : edge.source_edge_index,
        };
    });
}

std::string failure_message(const DagTopologyValidationReport & report) {
    if (report.issues.empty()) return "unknown topology validation failure";
    return report.issues.front().code + ": " + report.issues.front().message;
}

std::string effective_value(std::string_view local, std::string_view fallback) { return std::string(local.empty() ? fallback : local); }

size_t resolve_applied_ref(const DagNodeRef & ref, const std::unordered_map<std::string, size_t> & synthetic_node_ids) {
    if (ref.existing_node_id) return *ref.existing_node_id;
    return synthetic_node_ids.at(ref.synthetic_id);
}

class DagMutationApplier {
public:
    DagMutationApplier(DagGraph & graph, const DagMutationPlan & plan) : graph_(graph), plan_(plan), nodes_disabled_by_plan_(graph.node_count(), false) {
        result_.journal.plan_id = plan.plan_id;
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
            const auto effect_id = effective_value(mutation.effect_id, plan_.effect_id);
            const auto reason = effective_value(mutation.reason, plan_.reason);
            if (!effect_id.empty()) spec.attrs["effect_id"] = effect_id;
            if (!reason.empty()) spec.attrs["mutation_reason"] = reason;
            spec.attrs["synthetic_id"] = mutation.synthetic_id;
            const auto node_id = graph_.add_synthetic_node(spec);
            result_.synthetic_node_ids.emplace(mutation.synthetic_id, node_id);
            result_.journal.records.push_back(DagMutationRecord{
                .action = DagMutationAction::AddSyntheticNode,
                .effect_id = effect_id,
                .reason = reason,
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
                .effect_id = effective_value(mutation.effect_id, plan_.effect_id),
                .reason = effective_value(mutation.reason, plan_.reason),
                .node_id = mutation.node_id,
                .old_duration = old_duration,
                .new_duration = mutation.duration,
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
                .effect_id = plan_.effect_id,
                .reason = plan_.reason,
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
            const auto effect_id = effective_value(redirect.effect_id, effective_value(original.effect_id(), plan_.effect_id));
            const auto reason = effective_value(redirect.reason, plan_.reason);
            const auto new_edge_index = add_tracked_edge(src, dst, original.kind, effect_id, reason);
            result_.journal.records.push_back(DagMutationRecord{
                .action = DagMutationAction::RedirectEdge,
                .effect_id = effect_id,
                .reason = reason,
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
            .effect_id = plan_.effect_id,
            .reason = plan_.reason,
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
            .effect_id = plan_.effect_id,
            .reason = "disabled_with_node",
            .node_id = std::nullopt,
            .edge_index = edge_index,
        });
    }

    void add_edges() {
        for (const auto & addition : plan_.add_edges) {
            const auto src = resolve_applied_ref(addition.src, result_.synthetic_node_ids);
            const auto dst = resolve_applied_ref(addition.dst, result_.synthetic_node_ids);
            const auto effect_id = effective_value(addition.effect_id, plan_.effect_id);
            const auto reason = effective_value(addition.reason, plan_.reason);
            const auto edge_index = add_tracked_edge(src, dst, addition.kind, effect_id, reason);
            if (!edge_index) continue;
            result_.journal.records.push_back(DagMutationRecord{
                .action = DagMutationAction::AddEdge,
                .effect_id = effect_id,
                .reason = reason,
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
    result.journal.plan_id = plan.plan_id;
    result.journal.active_nodes_before = graph.active_node_count();
    result.journal.active_edges_before = graph.active_edge_count();
    result.journal.active_nodes_after = result.journal.active_nodes_before;
    result.journal.active_edges_after = result.journal.active_edges_before;
    return result;
}

} // namespace dag_mutation_detail

using dag_mutation_detail::build_prospective_graph;
using dag_mutation_detail::DagMutationApplier;
using dag_mutation_detail::empty_mutation_result;
using dag_mutation_detail::failure_message;
using dag_mutation_detail::validate_prospective_graph;

DagNodeRef DagNodeRef::existing(size_t node_id) { return DagNodeRef{ .existing_node_id = node_id, .synthetic_id = {} }; }

DagNodeRef DagNodeRef::synthetic(std::string synthetic_id) { return DagNodeRef{ .existing_node_id = std::nullopt, .synthetic_id = std::move(synthetic_id) }; }

bool DagMutationPlan::empty() const {
    return set_node_durations.empty() && disable_nodes.empty() && disable_edges.empty() && synthetic_nodes.empty() && add_edges.empty()
           && redirect_edges.empty();
}

DagMutationValidationError::DagMutationValidationError(const std::string & message, DagTopologyValidationReport report)
    : std::runtime_error(message),
      report_(std::move(report)) {}

std::string dag_mutation_action_name(DagMutationAction action) {
    switch (action) {
    case DagMutationAction::SetNodeDuration:
        return "set_node_duration";
    case DagMutationAction::DisableNode:
        return "disable_node";
    case DagMutationAction::DisableEdge:
        return "disable_edge";
    case DagMutationAction::AddSyntheticNode:
        return "add_synthetic_node";
    case DagMutationAction::AddEdge:
        return "add_edge";
    case DagMutationAction::RedirectEdge:
        return "redirect_edge";
    }
    return "unknown";
}

DagTopologyValidationReport validate_active_dag(const DagGraph & graph) { return dag_mutation_detail::validate_graph_storage(graph); }

DagTopologyValidationReport validate_dag_mutation_plan(const DagGraph & graph, const DagMutationPlan & plan) {
    return validate_prospective_graph(build_prospective_graph(graph, plan));
}

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
