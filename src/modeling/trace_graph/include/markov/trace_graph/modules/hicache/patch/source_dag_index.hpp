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

/** @brief Lightweight semantic fact retained for source-attribution evidence. */
struct HiCacheSourceFactNode {
    /** @brief Semantic fact identity from the canonical HiCache fact side-table. */
    size_t node_id = 0;
    /** @brief Executable DAG anchor only when separately proven by trace evidence. */
    std::optional<size_t> execution_anchor_node_id = std::nullopt;
    size_t event_index = 0;
    std::string event_name;
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
    uint64_t source_page_size = 0;
    uint64_t service_item_count = 0;
    uint64_t token_count = 0;
    uint64_t effective_token_count = 0;
    uint64_t completed_token_count = 0;
    bool completed_token_count_present = false;
    std::optional<bool> progress_ready = std::nullopt;
    std::optional<bool> write_back = std::nullopt;
    std::vector<uint64_t> operation_node_ids;
    std::vector<std::string> page_hashes;
    std::vector<std::string> source_page_hashes;
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

/** @brief Proven overlap between one semantic timing interval and a retained CPU gap. */
struct HiCacheCpuGapSlice {
    size_t owner_node_id = 0;
    size_t successor_node_id = 0;
    int logical_input_id = -1;
    uint64_t gap_start_us = 0;
    uint64_t gap_end_us = 0;
    uint64_t owned_start_us = 0;
    uint64_t owned_end_us = 0;

    [[nodiscard]] uint64_t owned_duration_us() const { return owned_end_us - owned_start_us; }
};

/** @brief Incremental overlap between two retained leaves on one CPU lane. */
struct HiCacheCpuOverlapSlice {
    size_t frontier_node_id = 0;
    size_t overlapping_node_id = 0;
    std::string frontier_event_name;
    std::string overlapping_event_name;
    uint64_t overlap_start_us = 0;
    uint64_t overlap_end_us = 0;

    [[nodiscard]] uint64_t duration_us() const { return overlap_end_us - overlap_start_us; }
};

/** @brief Exact same-thread decomposition of one observed call into leaves and gaps. */
struct HiCacheTimingIntervalOwnership {
    std::string status = "unresolved";
    uint64_t interval_start_us = 0;
    uint64_t interval_end_us = 0;
    uint64_t observed_duration_us = 0;
    uint64_t owned_node_duration_us = 0;
    uint64_t owned_gap_duration_us = 0;
    uint64_t overlapping_node_duration_us = 0;
    uint64_t max_node_overlap_us = 0;
    uint64_t uncovered_duration_us = 0;
    std::vector<size_t> owned_node_ids;
    std::vector<HiCacheCpuGapSlice> owned_gap_slices;
    std::vector<HiCacheCpuOverlapSlice> overlapping_node_slices;
    std::optional<size_t> start_anchor_node_id = std::nullopt;
    std::optional<size_t> completion_anchor_node_id = std::nullopt;
    std::string reason;
};

/** @brief Device-transfer nodes submitted by one observed host call and their existing readiness joins. */
struct HiCacheDeviceTransferClosure {
    std::string status = "unresolved";
    std::vector<size_t> transfer_node_ids;
    std::vector<size_t> completion_node_ids;
    std::vector<size_t> readiness_join_node_ids;
    uint64_t transfer_duration_us = 0;
    std::string reason;
};

/**
 * @brief Active adjacency plus narrow semantic identity indexes for HiCache attribution.
 *
 * Construction performs one executable-node pass and one fact-side-table pass. Event
 * arguments stay lazy: only Python-probe events carrying `args.fact` become semantic
 * fact records.
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
    [[nodiscard]] std::span<const HiCacheSourceFactNode> tail_context_facts() const { return tail_context_facts_; }
    [[nodiscard]] std::span<const size_t> nodes_for_fact_role(std::string_view role) const;
    [[nodiscard]] std::span<const size_t> nodes_for_request(std::string_view request_id) const;
    [[nodiscard]] std::span<const size_t> nodes_for_operation(std::string_view operation_id) const;
    [[nodiscard]] std::optional<size_t> cpu_boundary_at_or_before(std::string_view pid, std::string_view tid, uint64_t timestamp_us) const;
    [[nodiscard]] std::optional<size_t> cpu_boundary_at_or_after(std::string_view pid, std::string_view tid, uint64_t timestamp_us) const;
    [[nodiscard]] HiCacheTimingIntervalOwnership timing_interval_ownership(const HiCacheSourceFactNode & fact) const;
    [[nodiscard]] HiCacheTimingIntervalOwnership timing_interval_ownership(std::string_view pid, std::string_view tid, uint64_t start_us,
                                                                           uint64_t duration_us) const;
    /**
     * @brief Resolve the smallest explicit `hicache.control.*` interval enclosing a semantic fact.
     *
     * Control markers are intentionally not executable DAG nodes.  DagBuilder
     * retains their uncovered portions as `.self` leaves carrying a parent
     * identity.  This lookup reconstructs the marker interval from those
     * leaves, then applies the same exact leaf/gap ownership used by ordinary
     * timing observations.  The caller decides which named child leaves are a
     * modeled primitive and which `.self`/gap portions remain nuisance.
     */
    [[nodiscard]] std::optional<HiCacheTimingIntervalOwnership> enclosing_control_interval_ownership(const HiCacheSourceFactNode & fact,
                                                                                                     std::string_view control_event_name) const;
    [[nodiscard]] HiCacheDeviceTransferClosure device_transfer_closure(const HiCacheSourceFactNode & submission, std::string_view direction) const;
    [[nodiscard]] std::vector<HiCacheCpuGapSlice> project_foreground_gap_across_logical_input_lanes(std::span<const HiCacheCpuGapSlice> source_slices) const;

private:
    using NodeMap = std::unordered_map<std::string, std::vector<size_t>, core::TraceArgHash, std::equal_to<>>;

    struct ControlInterval {
        std::string pid;
        std::string tid;
        uint64_t start_us = 0;
        uint64_t end_us = 0;
    };

    using ControlIntervalMap = std::unordered_map<std::string, std::vector<ControlInterval>, core::TraceArgHash, std::equal_to<>>;

    const core::DagGraph & graph_;
    HiCacheSourceDagIndexStats stats_;
    std::vector<size_t> incoming_offsets_;
    std::vector<size_t> incoming_edge_ids_;
    std::vector<size_t> outgoing_offsets_;
    std::vector<size_t> outgoing_edge_ids_;
    std::vector<HiCacheSourceFactNode> fact_nodes_;
    std::vector<HiCacheSourceFactNode> tail_context_facts_;
    std::vector<size_t> fact_nodes_in_time_order_;
    std::unordered_map<size_t, size_t> fact_index_by_node_;
    std::unordered_map<size_t, size_t> tail_fact_index_by_node_;
    NodeMap nodes_by_fact_role_;
    NodeMap nodes_by_request_;
    NodeMap nodes_by_operation_;
    NodeMap cpu_nodes_by_lane_;
    ControlIntervalMap control_intervals_by_name_;
    std::unordered_map<int, std::vector<std::string>> cpu_lane_keys_by_logical_input_;

    [[nodiscard]] static std::span<const size_t> find_nodes(const NodeMap & index, std::string_view key);
    [[nodiscard]] static std::string cpu_lane_key(std::string_view pid, std::string_view tid);
};

} // namespace markov::trace_graph::modules::hicache::patch
