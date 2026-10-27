#pragma once

#include "markov/trace_graph/simulation/cpu_task_queues.hpp"
#include "markov/trace_graph/source_dag/index.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace markov::trace_graph::source_dag {

/** Zero-service queue placeholders strictly between owned device nodes.
 * Only a unique same-lane Stream chain without any other active dependency
 * is included. Caller ownership is not inferred from temporal proximity. */
[[nodiscard]] std::set<size_t> interior_stream_placeholders(const SourceDagIndex & source, const std::set<size_t> & owned);

/** Exact call envelope and its submission closure in the immutable graph.
 * Node ids are source-local. Stream/queue successors are not ownership;
 * sequential members of the same explicitly submitted CPU task are included.
 * external dependencies must still be reattached by the replacement planner.
 */
struct SubmittedCallRegion {
    size_t observation;
    source_dag::TimingIntervalOwnership caller;
    std::vector<size_t> submitted_nodes;
    std::vector<size_t> incoming_edges, outgoing_edges;
};

[[nodiscard]] SubmittedCallRegion observe_submitted_call_region(const SourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues,
                                                                size_t observation);

/** One complete task submitted by a call. Ordered by caller submission,
 * never by worker execution time; distinct tasks retain distinct dispatch.
 * Device nodes are owned by actual correlation edges from this task. */
struct QueuedCallTask {
    size_t task;
    std::vector<size_t> workers;
    std::set<size_t> devices;
};

/** Decompose the entire submitted closure, without measuring costs. Null
 * means partial task ownership, direct/ambiguous device submission, or an
 * unrecognized queue. Empty means a proven call with no queued work. */
[[nodiscard]] std::optional<std::vector<QueuedCallTask>>
    observe_queued_call_tasks(const SourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues, const SubmittedCallRegion & region);

/** Pair complete operator families in trace order. expected maps program
 * positions to framework Torch schemas; nested extra calls make that family
 * ambiguous instead of allowing a timing-nearest or partial correspondence. */
[[nodiscard]] std::map<size_t, size_t> match_operator_envelopes(const core::DagGraph & graph, std::span<const size_t> observations,
                                                                const std::map<size_t, std::string> & expected);

struct DeviceChain {
    size_t first, last;
};

/** A single observed stream chain without interleaved foreign work or event
 * waits. This establishes boundaries, not a cost or predicted lowering. */
[[nodiscard]] std::optional<DeviceChain> observe_serial_device_chain(const SourceDagIndex & source, const std::set<size_t> & devices);

} // namespace markov::trace_graph::source_dag
