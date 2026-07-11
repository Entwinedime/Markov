/**
 * @file
 * @brief Core DAG storage and mutation primitives for TraceGraph.
 */
#pragma once

#include "markov/trace_graph/core/trace_event.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace markov::trace_graph::core {

/**
 * @brief Semantic source of a hard DAG dependency.
 *
 * Every kind means that `dst` starts no earlier than `src` completes. The kind
 * remains explicit so diagnostics can attribute topology to its source rule.
 */
enum class DagEdgeKind : std::uint8_t { Sequential, Stream, Correlation, Sync, HCCL, HiCache, Mutation };

/** @brief Distinguishes input trace events from model-created nodes. */
enum class DagNodeKind : std::uint8_t { TraceEvent, Synthetic };

/** @brief Sparse provenance allocated only for model-created dependencies. */
struct DagEdgeProvenance {
    std::string effect_id;
    std::string reason;
};

/**
 * @brief One hard dependency in stable edge-index order.
 *
 * Base DAGs contain tens of millions of edges and normally have no mutation
 * provenance. Keeping optional strings behind one pointer avoids paying for two
 * `std::string` objects on every base edge.
 */
struct DagEdge {
    DagEdge() = default;
    DagEdge(size_t src, size_t dst, DagEdgeKind kind, std::string_view effect_id = {}, std::string_view reason = {});
    DagEdge(const DagEdge & other);
    DagEdge & operator=(const DagEdge & other);
    DagEdge(DagEdge &&) noexcept = default;
    DagEdge & operator=(DagEdge &&) noexcept = default;

    /** @brief Endpoints are node IDs, never `TraceEvent::index` values. */
    size_t src = 0;
    size_t dst = 0;
    DagEdgeKind kind = DagEdgeKind::Sequential;
    bool active = true;
    std::unique_ptr<DagEdgeProvenance> provenance;

    /** @brief Returns an empty view for ordinary base-DAG edges. */
    [[nodiscard]] std::string_view effect_id() const;

    /** @brief Returns an empty view when no diagnostic reason was attached. */
    [[nodiscard]] std::string_view reason() const;
};

/** @brief Execution identity and optional provenance for a synthetic node. */
struct DagSyntheticNodeSpec {
    std::string name;
    std::string category = "mutation";
    bool is_cpu = true;
    std::string lane_key = "SYNTHETIC";
    uint64_t duration = 0;
    std::unordered_map<std::string, std::string> attrs;
};

/**
 * @brief Executable instance of one duration event in the model graph.
 *
 * Trace nodes retain a stable `event_index`. Synthetic nodes receive their own
 * `TraceEvent`, so every node remains inspectable without an auxiliary fact table.
 * Lane strings are interned by `DagGraph`; nodes store only the compact lane ID.
 */
struct DagNode {
    static constexpr size_t kNoNode = std::numeric_limits<size_t>::max();

    size_t id = 0;
    DagNodeKind kind = DagNodeKind::TraceEvent;
    bool active = true;

    /** @brief Index into the owning graph's event vector. */
    size_t event_index = 0;

    /** @brief Logical input identity used for output and cross-rank grouping. */
    int gpu_id = 0;

    /** @brief CPU lanes use sequential edges; device lanes use stream edges. */
    bool is_cpu = true;

    /** @brief Interned logical lane owned by `DagGraph`. */
    size_t lane_id = 0;

    /** @brief Current execution duration consumed by topological simulation. */
    uint64_t duration = 0;

    /** @brief Immutable observed duration used as the baseline for model scaling. */
    uint64_t original_duration = 0;

    /**
     * @brief Observed idle gap after this node on the merged CPU lane.
     *
     * The gap advances wall time between sequential CPU nodes but is not part of
     * either node's execution duration. Keeping it typed prevents model modules
     * from accidentally scaling or double-counting idle time.
     */
    uint64_t cpu_gap_after = 0;

    /** @brief Relative start and completion times written by simulation. */
    uint64_t simulation_start = 0;
    uint64_t completion_time = 0;

    /**
     * @brief Earliest proven CPU submit timestamp for a device node.
     *
     * Zero means no submit anchor was observed and consumers must fall back to
     * the event timestamp. This value is execution semantics, not debug metadata.
     */
    uint64_t submit_ts = 0;

    /** @brief Same-rank successor used when cross-rank HCCL edges are added. */
    size_t hccl_successor_node_id = kNoNode;
};

/** @brief Counts derived from one immutable traversal of the active graph view. */
struct DagGraphSummaryStats {
    size_t active_node_count = 0;
    size_t active_trace_node_count = 0;
    size_t active_synthetic_node_count = 0;
    size_t active_edge_count = 0;
    std::unordered_map<std::string, size_t> edge_counts_by_kind;
};

/** @brief Storage reservation requested before graph construction or merge. */
struct DagGraphCapacity {
    size_t nodes = 0;
    size_t edges = 0;
};

/**
 * @brief Owns normalized events, compact nodes, hard edges, and simulation state.
 *
 * `DagGraph` enforces storage and endpoint invariants but does not infer trace
 * semantics. `DagBuilder` and model modules remain responsible for dependency
 * meaning. Node and edge indices stay stable; removals use tombstones.
 */
class DagGraph {
public:
    explicit DagGraph(std::vector<TraceEvent> events = {}, int gpu_id = 0);

    /** @brief Creates a node and interns its lane without adding dependencies. */
    size_t add_node(size_t event_index, bool is_cpu, std::string_view lane_key);

    /** @brief Creates a model node with an independent synthetic event identity. */
    size_t add_synthetic_node(const DagSyntheticNodeSpec & spec);

    /** @brief Reserves compact storage before constructing a large graph. */
    void reserve(const DagGraphCapacity & capacity);

    /** @brief Adds a hard dependency; callers own duplicate-edge prevention. */
    size_t add_edge(size_t src, size_t dst, DagEdgeKind kind, std::string_view effect_id = {}, std::string_view reason = {});

    /** @brief Tombstones an edge without changing stable edge indices. */
    void disable_edge(size_t edge_index);

    /** @brief Returns a read-only view of normalized and synthetic events. */
    [[nodiscard]] const std::vector<TraceEvent> & events() const { return events_; }

    /** @brief Returns mutable events for normalizers and explicit module boundaries. */
    std::vector<TraceEvent> & mutable_events() { return events_; }

    /** @brief Returns the stable node array. */
    [[nodiscard]] const std::vector<DagNode> & nodes() const { return nodes_; }

    /** @brief Returns the stable edge array. */
    [[nodiscard]] const std::vector<DagEdge> & edges() const { return edges_; }

    /** @brief Returns an event by event-vector index. */
    [[nodiscard]] const TraceEvent & event(size_t event_index) const;

    /** @brief Returns a mutable event by event-vector index. */
    TraceEvent & mutable_event(size_t event_index);

    /** @brief Resolves the event owned by a node. */
    [[nodiscard]] const TraceEvent & event_for_node(size_t node_id) const;

    /** @brief Resolves the mutable event owned by a node. */
    TraceEvent & mutable_event_for_node(size_t node_id);

    /** @brief Returns a node by stable node ID. */
    [[nodiscard]] const DagNode & node(size_t node_id) const;

    /** @brief Returns a mutable node by stable node ID. */
    DagNode & mutable_node(size_t node_id);

    /** @brief Returns an edge by stable edge index. */
    [[nodiscard]] const DagEdge & edge(size_t edge_index) const;

    /** @brief Returns a mutable edge by stable edge index. */
    DagEdge & mutable_edge(size_t edge_index);

    /** @brief Updates the sole authoritative execution duration for a node. */
    void set_node_duration(size_t node_id, uint64_t duration);

    /** @brief Returns the stable interned lane name for a node. */
    [[nodiscard]] std::string_view node_lane_key(size_t node_id) const;

    /** @brief Resolves a lane ID; throws when graph storage is inconsistent. */
    [[nodiscard]] std::string_view lane_key(size_t lane_id) const;

    /** @brief Looks up an already interned lane without allocating. */
    [[nodiscard]] std::optional<size_t> find_lane_id(std::string_view lane_key) const;

    /** @brief Returns total node storage, including tombstoned nodes. */
    [[nodiscard]] size_t node_count() const { return nodes_.size(); }

    /** @brief Returns nodes visible in the active graph view. */
    [[nodiscard]] size_t active_node_count() const;

    /** @brief Returns active nodes originating from input trace events. */
    [[nodiscard]] size_t active_trace_node_count() const;

    /** @brief Returns active synthetic nodes created by graph mutations. */
    [[nodiscard]] size_t active_synthetic_node_count() const;

    /** @brief Returns total edge storage, including tombstoned edges. */
    [[nodiscard]] size_t edge_count() const { return edges_.size(); }

    /** @brief Returns edges visible in the active graph view. */
    [[nodiscard]] size_t active_edge_count() const;

    /** @brief Checks for an active edge with matching endpoints, kind, and effect ID. */
    [[nodiscard]] bool has_active_edge(size_t src, size_t dst, DagEdgeKind kind, std::string_view effect_id = {}) const;

    /** @brief Returns reader records before normalization and filtering. */
    [[nodiscard]] size_t parsed_record_count() const { return parsed_record_count_; }

    /** @brief Stores reader records before normalization and filtering. */
    void set_parsed_record_count(size_t value) { parsed_record_count_ = value; }

#ifdef DEBUG
    /** @brief Returns the observed trace timestamp window for diagnostics. */
    [[nodiscard]] uint64_t real_e2e_time() const { return real_e2e_time_; }

    /** @brief Stores the observed trace timestamp window for diagnostics. */
    void set_real_e2e_time(uint64_t value) { real_e2e_time_ = value; }
#endif

    /** @brief Counts active dependencies by semantic edge kind. */
    [[nodiscard]] std::unordered_map<std::string, size_t> edge_counts_by_kind() const;

    /** @brief Computes all run-summary counts in one node pass and one edge pass. */
    [[nodiscard]] DagGraphSummaryStats summary_stats() const;

    /** @brief Returns the device ID assigned to this logical input. */
    [[nodiscard]] int gpu_id() const { return gpu_id_; }

    /** @brief Stores the critical-path duration produced by simulation. */
    void set_e2e_time(uint64_t value) { e2e_time_ = value; }

    /** @brief Returns the critical-path duration produced by simulation. */
    [[nodiscard]] uint64_t e2e_time() const { return e2e_time_; }

    /**
     * @brief Merges independently built per-rank graphs.
     *
     * Merge establishes only HCCL cross-rank constraints. Additional cross-rank
     * semantics belong in later modules with their own evidence and ownership.
     */
    static DagGraph merge(std::vector<DagGraph> graphs);

private:
    /** @brief File-local implementation of capacity-safe cross-rank graph merging. */
    class GraphMerger;

    /** @brief Returns a stable lane ID, inserting the name exactly once. */
    size_t intern_lane(std::string_view lane_key);

    /** @brief Parsed duration events plus synthetic events; excludes metadata and flows. */
    std::vector<TraceEvent> events_;
    std::vector<DagNode> nodes_;
    std::vector<DagEdge> edges_;

    /** @brief Millions of nodes normally share only a handful of lane names. */
    std::vector<std::string> lane_keys_;
    std::unordered_map<std::string, size_t, TraceArgHash, std::equal_to<>> lane_ids_;
    int gpu_id_ = 0;

    /** @brief Critical-path duration produced by topological simulation. */
    uint64_t e2e_time_ = 0;
#ifdef DEBUG
    /** @brief Observed input timestamp window, retained only for diagnostics. */
    uint64_t real_e2e_time_ = 0;
#endif

    /** @brief Reader record count retained across normalization for run summaries. */
    size_t parsed_record_count_ = 0;
};

} // namespace markov::trace_graph::core
