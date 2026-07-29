/**
 * @file
 * @brief Constructs the faithful base execution DAG from Chrome trace events.
 */
#pragma once

#include "markov/trace_graph/core/dag_graph.hpp"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef DEBUG
#include <cstdint>
#endif

namespace markov::trace_graph::core {

/**
 * @brief Converts normalized trace events into a simulatable base DAG.
 *
 * `DagBuilder` owns only dependencies justified by faithful replay:
 * - sequential ordering on the merged CPU lane;
 * - stream ordering on each device lane;
 * - runtime-launch to kernel correlation;
 * - event, stream, and device synchronization;
 * - model-execution synchronization anchors such as notify events.
 *
 * What-if policy belongs in a `SimulationModule` after construction, not in this builder.
 */
class DagBuilder {
public:
    /** @brief Sets maximum phase parallelism within one logical input. */
    explicit DagBuilder(size_t threads = 1);

    /** @brief Builds the base DAG for one rank and one logical trace input. */
    [[nodiscard]] DagGraph build(std::vector<TraceEvent> events, int gpu_id) const;

#ifdef DEBUG
    /** @brief Per-phase milliseconds retained only by Debug validation builds. */
    struct BuildTimings {
        uint64_t normalize_ms = 0;
        uint64_t create_nodes_ms = 0;
        uint64_t correlation_ms = 0;
        uint64_t sequential_ms = 0;
        uint64_t event_wait_ms = 0;
        uint64_t notify_wait_ms = 0;
        uint64_t model_execute_ms = 0;
        uint64_t stream_sync_ms = 0;
        uint64_t event_sync_ms = 0;
        uint64_t device_sync_ms = 0;
        uint64_t finalize_ms = 0;
        uint64_t real_e2e_ms = 0;
    };

    /** @brief Builds a DAG while recording Debug-only phase timings. */
    [[nodiscard]] DagGraph build_with_timings(std::vector<TraceEvent> events, int gpu_id, BuildTimings & timings) const;
#endif

private:
    /** @brief File-local staged implementation of event normalization. */
    class EventNormalizer;

    /** @brief File-local node creation and identity indexing implementation. */
    class NodeIndexer;

    /**
     * @brief Temporary lookup indices scoped to one build.
     *
     * Edge rules repeatedly query nodes by different trace identities. These indices
     * are invalid outside the graph and input from which they were constructed.
     */
    struct BuildIndex {
        /** @brief Interned lane ID to node IDs for ordering and sync lookup. */
        std::unordered_map<size_t, std::vector<size_t>> lane_to_nodes;
        /** @brief Runtime-to-device groups keyed by profiler correlation identities. */
        std::unordered_map<std::string, std::vector<size_t>> correlation_to_nodes;
        std::unordered_map<std::string, std::vector<size_t>> connection_to_nodes;
        /** @brief Record nodes with a confirmed event ID; missing IDs are never grouped. */
        std::unordered_map<std::string, std::vector<size_t>> event_id_to_nodes;
        /**
         * @brief Maps wrapper-visible raw stream handles to graph lanes.
         *
         * LD_PRELOAD and AscendCL wrappers expose raw handles rather than the lane
         * identity selected by `lane_key`; record events provide the bridge.
         */
        std::unordered_map<std::string, size_t> raw_stream_to_lane;
        /**
         * @brief Maps device-lane aliases to the interned lane selected by the builder.
         *
         * `streamId`, `Physic Stream Id`, and top-level `tid` can identify the same
         * device lane. Synchronization wrappers may expose only one of them.
         */
        std::unordered_map<std::string, size_t> stream_alias_to_lane;

        /** @brief Event-class caches that avoid rescanning the full graph per rule. */
        std::vector<size_t> event_record_nodes;
        std::vector<size_t> event_wait_nodes;
        std::vector<size_t> stream_sync_nodes;
        std::vector<size_t> event_sync_nodes;
        std::vector<size_t> device_sync_nodes;
        std::vector<size_t> notify_record_nodes;
        std::vector<size_t> notify_wait_nodes;
        size_t notify_wait_candidate_count = 0;
        std::vector<size_t> model_execute_nodes;
    };

    /** @brief Deduplicates executable events and removes nested CPU parents. */
    [[nodiscard]] static std::vector<TraceEvent> normalize_events(std::vector<TraceEvent> events);

    /** @brief Executes the ordered build pipeline through a direct or Debug timing profiler. */
    template <typename Profiler> [[nodiscard]] DagGraph build_impl(std::vector<TraceEvent> events, int gpu_id, Profiler & profiler) const;

#ifdef DEBUG
    /** @brief Computes the observed trace timestamp window for diagnostics. */
    static void set_real_e2e_time(DagGraph & graph);
#endif

    /**
     * @name Ordered build phases
     *
     * Node creation first builds all indices. Correlation, sequence, and sync phases
     * then add dependencies, after which synchronization-node durations are finalized.
     * The order is semantic and must remain centralized in `build_impl`.
     * @{
     */
    /** @brief Creates nodes and builds all indices required by later edge phases. */
    BuildIndex create_nodes(DagGraph & graph) const;

    /** @brief Computes a checked upper bound for all base-DAG edge rules. */
    [[nodiscard]] static size_t estimate_edge_capacity(const DagGraph & graph, const BuildIndex & index);

    /** @brief Adds CPU-runtime to device-kernel submission edges by correlation ID. */
    void add_correlation_edges(DagGraph & graph, BuildIndex & index) const;

    /** @brief Adds timestamp-ordered dependencies within every CPU and device lane. */
    void add_sequential_edges(DagGraph & graph, BuildIndex & index) const;

    /** @brief Adds Ascend event dependencies from `EVENT_RECORD` to `EVENT_WAIT`. */
    void add_event_wait_edges(DagGraph & graph, BuildIndex & index) const;

    /** @brief Connects model-execution synchronization anchors represented by notify events. */
    void add_notify_wait_edges(DagGraph & graph, BuildIndex & index) const;

    /** @brief Conservatively anchors device work to `MODEL_EXECUTE`. */
    void add_model_execute_edges(DagGraph & graph, BuildIndex & index) const;

    /** @brief Adds device-to-CPU wait edges for stream synchronization wrappers. */
    void add_stream_sync_edges(DagGraph & graph, BuildIndex & index) const;

    /** @brief Adds record-to-CPU wait edges for event synchronization wrappers. */
    void add_event_sync_edges(DagGraph & graph, BuildIndex & index) const;

    /** @brief Adds all-device-lane wait edges for device synchronization wrappers. */
    void add_device_sync_edges(DagGraph & graph, BuildIndex & index) const;

    /** @brief Replaces observed wait durations with fixed execution overhead. */
    void finalize_sync_nodes(DagGraph & graph, const BuildIndex & index) const;
    /** @} */

    size_t threads_ = 1;
};

} // namespace markov::trace_graph::core
