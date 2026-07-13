/**
 * @file
 * @brief Compact one-pass index over the active source DAG.
 */
#pragma once

#include "markov/trace_graph/core/dag_graph.hpp"
#include "markov/trace_graph/core/trace_event.hpp"
#include "markov/trace_graph/modules/hicache/fact.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace markov::trace_graph::modules::hicache::patch {

/** @brief Lightweight catalog fact retained for source-attribution evidence. */
struct HiCacheSourceFactNode {
    size_t node_id = 0;
    size_t event_index = 0;
    uint64_t timestamp_us = 0;
    uint64_t duration_us = 0;
    std::string pid;
    std::string tid;
    std::string target_id;
    std::string phase;
    std::string fact_class;
    std::string fact_role;
    std::string request_id;
    std::vector<std::string> batch_request_ids;
    std::string operation_id;
    std::string cache_scope;
    std::optional<uint64_t> object_node_id = std::nullopt;
    HiCacheTokenSpan full_path_span;
    uint64_t token_count = 0;
    uint64_t effective_token_count = 0;
    uint64_t completed_token_count = 0;
    std::optional<bool> progress_ready = std::nullopt;
    std::vector<uint64_t> operation_node_ids;
    std::vector<std::string> page_hashes;
};

/** @brief Aggregate sizes proving which source identities were indexed. */
struct HiCacheSourceDagIndexStats {
    std::string status = "not_built";
    size_t stored_node_count = 0;
    size_t active_node_count = 0;
    size_t active_edge_count = 0;
    size_t fact_node_count = 0;
    size_t workload_identity_fact_count = 0;
    size_t source_actual_fact_count = 0;
    size_t timing_observation_fact_count = 0;
    size_t malformed_fact_count = 0;
    size_t request_identity_count = 0;
    size_t operation_identity_count = 0;
    bool dag_patch_contract_ready = false;
    std::map<std::string, size_t> counts_by_fact_class;
    std::map<std::string, size_t> counts_by_fact_role;
    std::map<std::string, size_t> request_identity_counts_by_fact_role;
    std::map<std::string, size_t> operation_identity_counts_by_fact_role;
};

/**
 * @brief Active adjacency plus narrow semantic identity indexes for HiCache attribution.
 *
 * Construction performs one node pass and one edge pass. Event arguments stay lazy:
 * only identity keys advertised by a raw-key hint are parsed, and only Python-probe
 * events carrying `args.fact` become semantic fact records.
 */
class HiCacheSourceDagIndex {
public:
    explicit HiCacheSourceDagIndex(const core::DagGraph & graph);

    [[nodiscard]] const core::DagGraph & graph() const { return graph_; }
    [[nodiscard]] const HiCacheSourceDagIndexStats & stats() const { return stats_; }
    [[nodiscard]] const std::vector<HiCacheSourceFactNode> & fact_nodes() const { return fact_nodes_; }
    [[nodiscard]] std::span<const size_t> fact_nodes_in_time_order() const { return fact_nodes_in_time_order_; }

    [[nodiscard]] std::span<const size_t> incoming_edge_ids(size_t node_id) const;
    [[nodiscard]] std::span<const size_t> outgoing_edge_ids(size_t node_id) const;

    [[nodiscard]] const HiCacheSourceFactNode * fact_node(size_t node_id) const;
    [[nodiscard]] std::span<const size_t> nodes_for_fact_role(std::string_view role) const;
    [[nodiscard]] std::span<const size_t> nodes_for_request(std::string_view request_id) const;
    [[nodiscard]] std::span<const size_t> nodes_for_operation(std::string_view operation_id) const;

private:
    using NodeMap = std::unordered_map<std::string, std::vector<size_t>, core::TraceArgHash, std::equal_to<>>;

    const core::DagGraph & graph_;
    HiCacheSourceDagIndexStats stats_;
    std::vector<size_t> incoming_offsets_;
    std::vector<size_t> incoming_edge_ids_;
    std::vector<size_t> outgoing_offsets_;
    std::vector<size_t> outgoing_edge_ids_;
    std::vector<HiCacheSourceFactNode> fact_nodes_;
    std::vector<size_t> fact_nodes_in_time_order_;
    std::unordered_map<size_t, size_t> fact_index_by_node_;
    NodeMap nodes_by_fact_role_;
    NodeMap nodes_by_request_;
    NodeMap nodes_by_operation_;

    [[nodiscard]] static std::span<const size_t> find_nodes(const NodeMap & index, std::string_view key);
};

} // namespace markov::trace_graph::modules::hicache::patch
