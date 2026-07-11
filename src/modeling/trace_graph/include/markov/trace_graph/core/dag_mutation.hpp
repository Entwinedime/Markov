/**
 * @file
 * @brief Auditable DAG mutation plans, journals, and topology validation.
 */
#pragma once

#include "markov/trace_graph/core/dag_graph.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace markov::trace_graph::core {

/** @brief References either one stable existing node or one synthetic ID declared by the plan. */
struct DagNodeRef {
    std::optional<size_t> existing_node_id = std::nullopt;
    std::string synthetic_id{};

    [[nodiscard]] static DagNodeRef existing(size_t node_id);
    [[nodiscard]] static DagNodeRef synthetic(std::string synthetic_id);
};

/** @brief Declares a synthetic node with independent event identity. */
struct DagSyntheticNodeMutation {
    std::string synthetic_id{};
    DagSyntheticNodeSpec node;
    std::string effect_id{};
    std::string reason{};
};

/** @brief Adds one hard dependency between two node references. */
struct DagAddEdgeMutation {
    DagNodeRef src;
    DagNodeRef dst;
    DagEdgeKind kind = DagEdgeKind::Mutation;
    std::string effect_id{};
    std::string reason{};
};

/** @brief Replaces one active edge while optionally changing either endpoint. */
struct DagRedirectEdgeMutation {
    size_t edge_index = 0;
    std::optional<DagNodeRef> src = std::nullopt;
    std::optional<DagNodeRef> dst = std::nullopt;
    std::string effect_id{};
    std::string reason{};
};

/**
 * @brief A set of mutations that is validated as one prospective graph before apply.
 *
 * Validation failure leaves the graph untouched. Application does not maintain a
 * rollback copy; an unexpected implementation failure after validation may leave a
 * partially modified graph and must abort the enclosing workflow.
 */
struct DagMutationPlan {
    std::string plan_id{};
    std::string effect_id{};
    std::string reason{};
    std::vector<size_t> disable_nodes{};
    std::vector<size_t> disable_edges{};
    std::vector<DagSyntheticNodeMutation> synthetic_nodes{};
    std::vector<DagAddEdgeMutation> add_edges{};
    std::vector<DagRedirectEdgeMutation> redirect_edges{};

    [[nodiscard]] bool empty() const;
};

/** @brief Action kinds emitted to the applied-mutation journal. */
enum class DagMutationAction : std::uint8_t { DisableNode, DisableEdge, AddSyntheticNode, AddEdge, RedirectEdge };

/** @brief One mutation that materially changed graph storage or activity. */
struct DagMutationRecord {
    DagMutationAction action = DagMutationAction::DisableNode;
    std::string effect_id{};
    std::string reason{};
    std::optional<size_t> node_id = std::nullopt;
    std::optional<size_t> edge_index = std::nullopt;
    std::optional<size_t> replaced_edge_index = std::nullopt;
    std::optional<size_t> src = std::nullopt;
    std::optional<size_t> dst = std::nullopt;
};

/** @brief Active graph counts and concrete actions before and after plan application. */
struct DagMutationJournal {
    std::string plan_id{};
    size_t active_nodes_before = 0;
    size_t active_nodes_after = 0;
    size_t active_edges_before = 0;
    size_t active_edges_after = 0;
    std::vector<DagMutationRecord> records{};
};

/** @brief Structured problem found in an active or prospective topology. */
struct DagTopologyIssue {
    std::string code{};
    std::string message{};
    std::vector<size_t> node_ids{};
    std::vector<size_t> edge_indices{};
};

/** @brief Endpoint, duplicate mutation-edge, and cycle validation result. */
struct DagTopologyValidationReport {
    size_t active_node_count = 0;
    size_t active_edge_count = 0;
    std::vector<size_t> cycle_nodes{};
    std::vector<DagTopologyIssue> issues{};

    [[nodiscard]] bool ok() const { return issues.empty(); }
};

/** @brief Applied journal and synthetic-ID mapping, plus Debug post-apply validation. */
struct DagMutationResult {
    DagMutationJournal journal;
#ifdef DEBUG
    DagTopologyValidationReport topology;
#endif
    std::unordered_map<std::string, size_t> synthetic_node_ids;
};

/** @brief Carries the complete report when prospective or Debug post-apply validation fails. */
class DagMutationValidationError : public std::runtime_error {
public:
    DagMutationValidationError(const std::string & message, DagTopologyValidationReport report);

    [[nodiscard]] const DagTopologyValidationReport & report() const { return report_; }

private:
    DagTopologyValidationReport report_;
};

/** @brief Returns the stable artifact name for a mutation action. */
[[nodiscard]] std::string dag_mutation_action_name(DagMutationAction action);

/** @brief Validates the current active graph without modifying it. */
[[nodiscard]] DagTopologyValidationReport validate_active_dag(const DagGraph & graph);

/** @brief Validates a complete plan against a prospective graph without modifying storage. */
[[nodiscard]] DagTopologyValidationReport validate_dag_mutation_plan(const DagGraph & graph, const DagMutationPlan & plan);

/**
 * @brief Validates the prospective graph, applies the plan, and returns its journal.
 *
 * Non-empty plans always receive pre-apply topology validation. Debug builds also
 * validate the materialized graph after apply to detect divergence between the
 * prospective and concrete mutation implementations.
 */
[[nodiscard]] DagMutationResult apply_dag_mutation_plan(DagGraph & graph, const DagMutationPlan & plan);

} // namespace markov::trace_graph::core
