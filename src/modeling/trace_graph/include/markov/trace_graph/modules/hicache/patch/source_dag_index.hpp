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
    uint64_t source_page_size = 0;
    uint64_t service_item_count = 0;
    std::optional<uint64_t> storage_existing_page_count;
    std::optional<uint64_t> storage_new_page_count;
    uint64_t token_count = 0;
    uint64_t effective_token_count = 0;
    uint64_t completed_token_count = 0;
    bool completed_token_count_present = false;
    std::optional<bool> progress_ready = std::nullopt;
    std::optional<uint64_t> host_available_tokens_at_return;
    std::optional<bool> write_back = std::nullopt;
    std::vector<uint64_t> operation_node_ids;
    std::vector<std::string> page_hashes;
};

/** @brief Proven overlap between one semantic timing interval and a retained CPU gap. */
struct HiCacheCpuGapSlice {
    size_t owner_node_id = 0;
    size_t successor_node_id = 0;
    uint64_t gap_start_us = 0;
    uint64_t gap_end_us = 0;
    uint64_t owned_start_us = 0;
    uint64_t owned_end_us = 0;

    [[nodiscard]] uint64_t owned_duration_us() const { return owned_end_us - owned_start_us; }
};

/** @brief Exact same-thread decomposition of one observed call into leaves and gaps. */
struct HiCacheTimingIntervalOwnership {
    std::string status = "unresolved";
    uint64_t interval_start_us = 0;
    uint64_t interval_end_us = 0;
    uint64_t observed_duration_us = 0;
    uint64_t owned_node_duration_us = 0;
    uint64_t owned_gap_duration_us = 0;
    bool has_node_overlap = false;
    uint64_t uncovered_duration_us = 0;
    std::vector<size_t> owned_node_ids;
    std::vector<HiCacheCpuGapSlice> owned_gap_slices;
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
struct HiCacheDeviceStreamOrder {
    std::vector<size_t> nodes;
    std::unordered_map<size_t, size_t> positions;
    [[nodiscard]] std::optional<size_t> previous(size_t node) const;
    [[nodiscard]] std::optional<size_t> next(size_t node) const;
};

class HiCacheSourceDagIndex {
public:
    explicit HiCacheSourceDagIndex(const core::DagGraph & graph);

    [[nodiscard]] const core::DagGraph & graph() const { return graph_; }
    [[nodiscard]] const std::vector<HiCacheSourceFactNode> & fact_nodes() const { return fact_nodes_; }

    [[nodiscard]] std::span<const size_t> incoming_edge_ids(size_t node_id) const;
    [[nodiscard]] std::span<const size_t> outgoing_edge_ids(size_t node_id) const;
    /** Unique order proven solely by active Stream dependencies. The sample
     * identifies rank/lane even after removal; an empty resource has no nodes.
     * Transitive shortcuts are allowed; disconnected, branching or cyclic work is not. */
    [[nodiscard]] const HiCacheDeviceStreamOrder & device_stream_order(size_t sample) const;

    [[nodiscard]] const HiCacheSourceFactNode * fact_node(size_t node_id) const;
    [[nodiscard]] std::span<const HiCacheSourceFactNode> tail_context_facts() const { return tail_context_facts_; }
    [[nodiscard]] std::span<const size_t> nodes_for_fact_role(std::string_view role) const;
    [[nodiscard]] std::span<const size_t> nodes_for_request(std::string_view request_id) const;
    [[nodiscard]] std::span<const size_t> cpu_nodes_on_lane(std::string_view pid, std::string_view tid) const;
    /** Borrow the time-ordered candidate slice for [begin_us, end_us).
     * Prefix maxima retain long enclosing leaves; callers filter individual
     * ends because shorter leaves in the slice may have already finished. */
    [[nodiscard]] std::span<const size_t> cpu_interval_candidates(std::string_view pid, std::string_view tid, uint64_t begin_us, uint64_t end_us) const;
    /** Exact, unique start only; containment and adjacent end points are not substitutes. */
    [[nodiscard]] std::optional<size_t> cpu_node_starting_at(std::string_view pid, std::string_view tid, uint64_t timestamp_us) const;
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
    std::vector<size_t> incoming_offsets_;
    std::vector<size_t> incoming_edge_ids_;
    std::vector<size_t> outgoing_offsets_;
    std::vector<size_t> outgoing_edge_ids_;
    std::vector<HiCacheSourceFactNode> fact_nodes_;
    std::vector<HiCacheSourceFactNode> tail_context_facts_;
    std::unordered_map<size_t, size_t> fact_index_by_node_;
    std::unordered_map<size_t, size_t> tail_fact_index_by_node_;
    NodeMap nodes_by_fact_role_;
    NodeMap nodes_by_request_;
    NodeMap cpu_nodes_by_lane_;
    NodeMap device_nodes_by_direction_;
    /** @brief Prefix maxima of immutable observed event ends, aligned with sorted lane nodes. */
    std::unordered_map<std::string, std::vector<uint64_t>, core::TraceArgHash, std::equal_to<>> cpu_prefix_end_us_by_lane_;
    ControlIntervalMap control_intervals_by_name_;
    mutable std::map<std::pair<int, size_t>, HiCacheDeviceStreamOrder> device_stream_orders_;

    [[nodiscard]] static std::span<const size_t> find_nodes(const NodeMap & index, std::string_view key);
    [[nodiscard]] static std::string cpu_lane_key(std::string_view pid, std::string_view tid);
};

} // namespace markov::trace_graph::modules::hicache::patch
