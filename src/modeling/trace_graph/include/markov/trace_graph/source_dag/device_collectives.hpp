#pragma once

#include "markov/trace_graph/source_dag/call_regions.hpp"
#include "markov/trace_graph/structure/hicache/collective.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace markov::trace_graph::source_dag {

/** Recognize the native queued submission implementation; unknown task
 * names remain unknown and cannot be pooled into reduction CPU overhead. */
[[nodiscard]] std::optional<structure::hicache::CollectiveTaskKind> collective_task_kind(std::string_view submission_name);

/** One rank's observed ordinary all-reduce. Shape is a packed token-major
 * matrix, not a target shape. Phase/layer ownership and all peer descriptors
 * have been corroborated; no communication duration is a service estimate. */
struct DeviceCollectiveCall {
    SubmittedCallRegion region;
    PhaseCallPosition position;
    std::string group, dtype, role, callsite;
    uint64_t group_sequence, layer_index, rows, width, element_bytes;
    int64_t global_rank;
    std::vector<int64_t> members;
    // Original observation indices on all ranks, in process-group order.
    std::vector<size_t> peer_observations;
    std::vector<size_t> communication_nodes;
    // A same-process/group ordinary call returned before its own raw device
    // completion. WorkHCCL's group-level blocking-wait option is immutable.
    bool nonblocking_group = false;
    structure::hicache::CollectiveTaskKind transfer = structure::hicache::CollectiveTaskKind::Reduce;
};

struct DeviceCollectiveAudit {
    std::vector<DeviceCollectiveCall> calls;
    std::map<std::string, size_t> issues;
};

struct CollectiveCallerInterval {
    size_t step;
    uint64_t begin_us, end_us;
    std::optional<size_t> submission;
};

/** Corroborate the ordinary caller rule with complete queued-task ownership
 * and native submission order, then partition its observation at enqueue
 * completion boundaries. Unknown protocols yield no partial role assignment.
 * Boundaries describe CPU measurement intervals, not target timestamps. */
[[nodiscard]] std::optional<std::vector<CollectiveCallerInterval>>
    observe_collective_caller_intervals(const modules::hicache::patch::HiCacheSourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues,
                                        const DeviceCollectiveCall & call, std::span<const QueuedCallTask> tasks);

/** Source-only attribution for the current ordinary Qwen3 TP path. Unknown
 * layouts, missing peers and ambiguous submission ownership are issues, not
 * zero work or inferred peer calls. This does not prove target dispatch.
 * When requested, diagnostics receives the same extraction's detailed report;
 * neither output mutates the graph or changes attribution policy. The source
 * index and queues describe the same immutable graph throughout this call. */
[[nodiscard]] DeviceCollectiveAudit observe_device_collectives(const modules::hicache::patch::HiCacheSourceDagIndex & source,
                                                               const simulation::detail::CpuTaskQueues & queues,
                                                               const modules::hicache::HiCachePhaseObservationAudit & phases,
                                                               nlohmann::json * diagnostics = nullptr);

} // namespace markov::trace_graph::source_dag
