#pragma once

#include "markov/trace_graph/core/dag_graph.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace markov::trace_graph::source_dag {

namespace detail {
class OperatorEnvelopeIndex;
}

/** Observed overlap with a same-thread gap, not an additional service cost. */
struct CpuGapSlice {
    size_t owner_node_id = 0;
    size_t successor_node_id = 0;
    uint64_t gap_start_us = 0;
    uint64_t gap_end_us = 0;
    uint64_t owned_start_us = 0;
    uint64_t owned_end_us = 0;

    [[nodiscard]] uint64_t owned_duration_us() const { return owned_end_us - owned_start_us; }
};

/** Exact same-thread decomposition into retained leaves and residual gaps.
 * Node identities and anchors belong to the immutable source graph. */
struct TimingIntervalOwnership {
    std::string status = "unresolved";
    uint64_t interval_start_us = 0;
    uint64_t interval_end_us = 0;
    uint64_t observed_duration_us = 0;
    uint64_t owned_node_duration_us = 0;
    uint64_t owned_gap_duration_us = 0;
    bool has_node_overlap = false;
    uint64_t uncovered_duration_us = 0;
    std::vector<size_t> owned_node_ids;
    std::vector<CpuGapSlice> owned_gap_slices;
    std::optional<size_t> start_anchor_node_id = std::nullopt;
    std::optional<size_t> completion_anchor_node_id = std::nullopt;
    std::string reason;
};

struct DeviceStreamOrder {
    std::vector<size_t> nodes;
    std::unordered_map<size_t, size_t> positions;

    [[nodiscard]] std::optional<size_t> previous(size_t node) const;
    [[nodiscard]] std::optional<size_t> next(size_t node) const;
};

/** Borrowed stream ordinals between proven caller submissions. Unanchored
 * ordinals have no inferred timestamp; the consumer decides their legality. */
struct DeviceSubmissionCut {
    std::optional<size_t> last_before, first_after;
    std::span<const size_t> unanchored_ordinals;
};

/** Adjacency, CPU intervals and submitted device work in one source graph.
 * The graph must outlive the index and remain unchanged, including node
 * activity and dependencies. Rebuild after mutation. No framework facts,
 * target work, costs or predicted execution time are interpreted here.
 * CPU and adjacency caches materialize independently on first query;
 * an instance, including its device caches, is not shared between threads. */
class SourceDagIndex {
public:
    explicit SourceDagIndex(const core::DagGraph & graph);
    [[nodiscard]] const detail::OperatorEnvelopeIndex & operator_envelopes() const;

    [[nodiscard]] const core::DagGraph & graph() const { return graph_; }
    [[nodiscard]] std::span<const size_t> incoming_edge_ids(size_t node_id) const;
    [[nodiscard]] std::span<const size_t> outgoing_edge_ids(size_t node_id) const;

    /** Unique order from active Stream dependencies. The sample identifies
     * rank/lane even if removed; disconnected, branching or cyclic work fails. */
    [[nodiscard]] const DeviceStreamOrder & device_stream_order(size_t sample) const;
    /** Caller anchors from active Correlation edges, aligned with stream order.
     * Empty entries mean no proven submission, never timestamp inference. */
    [[nodiscard]] const std::vector<std::vector<size_t>> & device_submission_anchors(size_t sample, const std::string & pid, const std::string & tid) const;
    /** Resolve all correlated anchors around a source-time cut. Overlap or
     * disagreement with stream order fails. Retained pre-window enqueue
     * coordinates may prove submission without restoring CPU execution. */
    [[nodiscard]] DeviceSubmissionCut device_submission_cut(size_t sample, const std::string & pid, const std::string & tid, uint64_t at_us) const;
    /** All proven anchors lie in [begin_us, end_us). Worker submission can
     * occur after caller return; this query concerns the caller envelope. */
    [[nodiscard]] bool device_submitted_in_call(size_t node, const std::string & pid, const std::string & tid, uint64_t begin_us, uint64_t end_us) const;

    [[nodiscard]] std::span<const size_t> cpu_nodes_on_lane(std::string_view pid, std::string_view tid) const;
    /** Time-ordered candidates for [begin_us, end_us). Prefix maxima preserve
     * enclosing leaves; individual ends still require consumer filtering. */
    [[nodiscard]] std::span<const size_t> cpu_interval_candidates(std::string_view pid, std::string_view tid, uint64_t begin_us, uint64_t end_us) const;
    /** Exact unique start; containment and adjacent endpoints do not qualify. */
    [[nodiscard]] std::optional<size_t> cpu_node_starting_at(std::string_view pid, std::string_view tid, uint64_t timestamp_us) const;
    [[nodiscard]] std::optional<size_t> cpu_boundary_at_or_before(std::string_view pid, std::string_view tid, uint64_t timestamp_us) const;
    [[nodiscard]] std::optional<size_t> cpu_boundary_at_or_after(std::string_view pid, std::string_view tid, uint64_t timestamp_us) const;
    [[nodiscard]] TimingIntervalOwnership timing_interval_ownership(std::string_view pid, std::string_view tid, uint64_t start_us, uint64_t duration_us) const;
    /** Also retain zero-duration correlated submissions in [start, end).
     * Bare gates are not submissions; a zero-width interval owns no points. */
    [[nodiscard]] TimingIntervalOwnership call_interval_ownership(std::string_view pid, std::string_view tid, uint64_t start_us, uint64_t duration_us) const;

protected:
    using NodeMap = std::unordered_map<std::string, std::vector<size_t>, core::TraceArgHash, std::equal_to<>>;
    [[nodiscard]] static std::span<const size_t> find_nodes(const NodeMap & index, std::string_view key);

private:
    const core::DagGraph & graph_;
    mutable std::shared_ptr<const detail::OperatorEnvelopeIndex> operator_envelopes_;
    mutable std::shared_ptr<const core::DagAdjacency> adjacency_;
    mutable std::shared_ptr<const core::CpuIntervalIndex> cpu_index_;
    mutable std::map<std::pair<int, size_t>, DeviceStreamOrder> device_stream_orders_;
    mutable std::map<std::tuple<int, size_t, std::string, std::string>, std::vector<std::vector<size_t>>> device_submission_anchors_;

    struct DeviceSubmissionTimeline {
        // A zero-duration anchor at the cut belongs after it.
        std::vector<std::pair<std::pair<uint64_t, bool>, size_t>> finishes;
        std::vector<std::pair<uint64_t, size_t>> starts;
        std::vector<size_t> last_before_prefix, first_after_suffix, unanchored;
    };
    mutable std::map<std::tuple<int, size_t, std::string, std::string>, DeviceSubmissionTimeline> device_submission_timelines_;

    [[nodiscard]] const core::DagAdjacency & adjacency() const;
    [[nodiscard]] const core::CpuIntervalIndex & cpu_index() const;
    [[nodiscard]] static std::string cpu_lane_key(std::string_view pid, std::string_view tid);
};

} // namespace markov::trace_graph::source_dag
