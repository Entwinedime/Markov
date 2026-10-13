#pragma once

#include "markov/trace_graph/modules/hicache/layer_waits.hpp"
#include "markov/trace_graph/source_dag/call_regions.hpp"
#include "markov/trace_graph/structure/operation.hpp"
#include "markov/trace_graph/structure/tensor_program.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <set>
#include <span>
#include <vector>

namespace markov::trace_graph::source_dag {

/** Measure a source CPU node without absorbing device waiting. The caller
 * supplies a CPU node from the immutable source graph. Unknown/multiple
 * synchronization producers or a producer still running at API entry yield
 * no sample. Returned microseconds use this capture's CPU correction, when
 * supplied; otherwise they remain profiled measurements. */
[[nodiscard]] std::optional<uint64_t> observe_nonblocking_cpu_service(const modules::hicache::patch::HiCacheSourceDagIndex & source, size_t node);

/** Measure a raw device EVENT_WAIT only when its recorded event completed
 * before the wait began. Logical/synthetic waits and unresolved or blocking
 * waits have no service sample. Never return the builder's fixed sync cost.
 * Returned microseconds retain the raw event's sub-microsecond precision. */
[[nodiscard]] std::optional<double> observe_device_event_wait_service(const modules::hicache::patch::HiCacheSourceDagIndex & source, size_t node);

struct WorkerLaunchObservation {
    uint64_t launch_us = 0, launch_residual_us = 0;
    uint64_t return_us = 0, return_residual_us = 0;
};

/** Measure one complete FIFO task with one proven device launch point.
 * Input worker ids may be unordered; missing/overlapping members, waits or
 * multiple launch points yield no sample. A device-side event wait does not
 * invalidate nonblocking CPU submission measurements; its elapsed wait is
 * never measured as device service here. Durations are microseconds. */
[[nodiscard]] std::optional<WorkerLaunchObservation> observe_worker_launch(const modules::hicache::patch::HiCacheSourceDagIndex & source,
                                                                           const simulation::detail::CpuTaskQueues & queues, std::vector<size_t> workers,
                                                                           const std::set<size_t> & devices);

/** Measure one already-attributed queued task, not the enclosing call. A
 * single launch yields independent launch/return scopes; a CPU-only task
 * yields worker_service and worker_residual_interval. Dispatch is separate.
 * No caller or device service is emitted. Tasks with multiple launch points
 * need an explicit native program rather than a pooled worker sample. */
[[nodiscard]] nlohmann::json queued_task_observations(const modules::hicache::patch::HiCacheSourceDagIndex & source,
                                                      const simulation::detail::CpuTaskQueues & queues, const QueuedCallTask & task,
                                                      const structure::OperationWork & work, bool cpu_service_applied);

/** Extract a tensor call's separately attributable device, submit, return,
 * worker and dispatch measurements. Partial scopes remain partial: never
 * replace an unobserved worker or CPU service with the outer wall time.
 * measured_devices prevents overlapping envelopes from counting service twice.
 * include_caller_scopes=false retains device/worker samples but omits caller
 * CPU scopes when its envelope includes separately accounted preparation.
 * A single-resource sync is measurable only when its observed producer was
 * already complete on API entry; use its original interval, not replay cost.
 * Caller scopes use the last proven submission boundary independently of
 * worker service eligibility. Multiple tasks do not become one worker sample;
 * dispatch for one complete task remains measurable if its service is not.
 * Within a proven single task, launch and return CPU segments are admitted
 * independently. Unmeasurable CPU segments do not discard proven native
 * device samples; missing segments remain absent rather than zero-valued.
 * Conversely, a missing serial device-chain proof does not discard caller
 * or single-launch worker samples. It prevents aggregated device service and
 * blocking-return measurements, which require that completion proof.
 * Explicit native children require one device event per launch and matching
 * operator/engine labels in order, in addition to the source program's dtype
 * and geometry evidence. Equal launch counts alone do not admit those costs.
 * A proven caller synchronization to this operation's last serial device node
 * additionally yields a blocking_return_interval after both submission and
 * device completion. It includes host reaction/residual time, not pure CPU;
 * unknown/multiple waits or extra submissions do not produce this scope.
 * Caller-side scalar/context readback is different: a proven stream wait then
 * synchronous memcpy yields blocking_readback_interval and pre-wait CPU work,
 * not worker/device samples. That interval includes the synchronous transfer.
 * post_sync_readback_interval uses only host timestamps, from synchronization
 * return to the call's end. It includes the copy and caller tail, not the
 * synchronization service or producer waiting; it cannot replace the full
 * blocking_readback_interval without a separately estimated sync service.
 */
[[nodiscard]] nlohmann::json tensor_operator_observations(const modules::hicache::patch::HiCacheSourceDagIndex & source,
                                                          const simulation::detail::CpuTaskQueues & queues, const structure::OperationWork & work,
                                                          size_t observation, std::set<size_t> & measured_devices, bool cpu_service_applied,
                                                          bool include_caller_scopes = true, std::span<const structure::OperationWork> device_launches = {});

/** Append samples for the matched operators of an already-described program.
 * Program indices and observation indices remain source-local. Device ownership
 * accumulates in measured_devices across calls; source and program are unchanged.
 * Cold preparation can exclude all caller scopes or just one program index,
 * without discarding independently measurable device/worker scopes. */
void append_tensor_program_observations(const modules::hicache::patch::HiCacheSourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues,
                                        std::span<const structure::TensorOperation> program, const std::map<size_t, size_t> & matched,
                                        std::set<size_t> & measured_devices, bool cpu_service_applied, nlohmann::json & rows, bool include_caller_scopes = true,
                                        std::optional<size_t> omitted_caller_operation = std::nullopt);

/** Measure preparation/return intervals between an outer program's consumers.
 * Such programs alternate CPU preparation and a tensor/layer-wait consumer,
 * followed by a CPU tail. All boundaries must match before any rows are returned;
 * ambiguous, overlapping or unmeasured intervals return an empty array, not zeros.
 * Layer waits are source boundaries only, never extra costs of the outer call.
 * The caller separately excludes cold preparation and incomplete native mappings. */
[[nodiscard]] nlohmann::json tensor_program_outer_observations(const modules::hicache::patch::HiCacheSourceDagIndex & source,
                                                               const SubmittedCallRegion & region, std::span<const structure::TensorOperation> program,
                                                               const std::map<size_t, size_t> & matched, bool cpu_service_applied,
                                                               std::span<const modules::hicache::HiCacheLayerWaitCall> layer_waits = {});

/** Append CPU and residual scopes only if the complete interval is owned and
 * contains no unaccounted submission or unmeasured synchronization. An optional
 * permitted_submission names the single enqueue whose worker/device costs are
 * owned separately by the caller. Every other submission invalidates the sample. A known
 * single-resource synchronization must already be complete at API entry.
 * False appends nothing.
 * Source interval coordinates and operation identity establish sample identity.
 */
bool append_cpu_interval_observations(const modules::hicache::patch::HiCacheSourceDagIndex & source, const core::TraceEvent & outer,
                                      const structure::OperationWork & work, uint64_t begin, uint64_t end, bool corrected, nlohmann::json & rows,
                                      std::optional<size_t> permitted_submission = std::nullopt);

} // namespace markov::trace_graph::source_dag
