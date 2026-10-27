#pragma once

#include "markov/trace_graph/adapters/frameworks/sglang/allocator_program.hpp"
#include "markov/trace_graph/adapters/frameworks/sglang/attention_program.hpp"
#include "markov/trace_graph/modules/hicache/phase_observation.hpp"
#include "markov/trace_graph/source_dag/call_regions.hpp"
#include "markov/trace_graph/source_dag/decode_allocation.hpp"
#include "markov/trace_graph/structure/hicache/collective.hpp"
#include "markov/trace_graph/structure/phase_shape.hpp"
#include "markov/trace_graph/structure/tensor_program.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace markov::trace_graph::adapters::sglang {

struct PhaseCallPosition {
    size_t phase_index;
    std::optional<uint64_t> decode_iteration;
    bool operator==(const PhaseCallPosition &) const = default;
};

struct PhaseCallPositions {
    std::map<size_t, std::optional<PhaseCallPosition>> nodes;
    std::map<size_t, PhaseCallPosition> calls;
};

/** Source execution nodes establish request/round ownership; shared or
 * contradictory assignments are unavailable, never resolved by proximity.
 * Calls without submitted work, including CPU-only views, use exact
 * same-thread forward containment instead;
 * this establishes semantics, not zero execution cost. Prepare once from the
 * immutable audit and pass the same positions to every call family. */
[[nodiscard]] PhaseCallPositions phase_call_positions(const modules::hicache::HiCachePhaseObservationAudit & phases);
[[nodiscard]] std::optional<PhaseCallPosition> call_phase_position(const source_dag::SubmittedCallRegion & region, const PhaseCallPositions & positions);

/** Decode ordinary metadata inputs without measuring costs. Capture, SWA,
 * MLA, speculative/multi-request work and incomplete evidence are unavailable.
 * Context is the framework's recorded scalar, not total prompt length or
 * a value inferred from page-table width. No target data is consumed. */
[[nodiscard]] std::optional<adapters::sglang::AttentionMetadataInputs> observed_attention_metadata_inputs(const core::TraceEvent & descriptor);

struct AllocatorRegion {
    source_dag::SubmittedCallRegion work;
    modules::hicache::model::HiCacheAllocatorPhase phase;
    std::optional<bool> allocated;
    // Original Torch envelopes, not additional execution or additive costs.
    std::vector<size_t> operator_observations;
};

/** Measured call semantics and corroborating operator envelopes. A program
 * alone is not observation evidence: matched_operators requires actual device
 * submissions or a proven synchronous readback. Shared by cost export and reporting. */
struct ObservedAllocatorProgram {
    AllocatorRegion region;
    std::optional<std::vector<structure::TensorOperation>> program;
    std::map<size_t, size_t> matched_operators;
    std::optional<size_t> fused_position;
    bool has_preparation = false;
    bool complete_tensor_mapping = false;
};

[[nodiscard]] std::vector<ObservedAllocatorProgram> observe_allocator_programs(const source_dag::SourceDagIndex & source,
                                                                               const simulation::detail::CpuTaskQueues & queues);

struct AttentionDescriptorPosition {
    bool prelude;
    size_t observation;
};

struct AttentionRegion {
    source_dag::SubmittedCallRegion work;
    std::string phase, sampling, status;
    std::optional<AttentionDescriptorPosition> descriptor;
    // Original Torch CPU operator envelopes, including nested operators.
    // Indices address runtime_observations; durations are not additive costs.
    std::vector<size_t> operator_observations;
};

/** Per-forward page-table preparation, outside per-layer attention calls.
 * The descriptor and enclosed Torch operators address source observations;
 * an envelope establishes ownership, not another additive service sample. */
struct AttentionMetadataRegion {
    source_dag::SubmittedCallRegion work;
    std::vector<size_t> operator_observations;
};

struct DenseRegion {
    source_dag::SubmittedCallRegion work;
    std::vector<size_t> operator_observations;
    std::string implementation;
    structure::DenseShape shape;
    // Optional enclosing framework descriptor supplies semantic ownership;
    // work remains the exact inner operation interval, never the whole owner.
    std::optional<size_t> descriptor_observation;
};

/** Measured 2-D nonquantized linear, fused gate or RMSNorm geometry/layout.
 * No token-axis inference: rows are observed, not classified by size. Unsupported
 * ranks or missing descriptors are omitted, not flattened heuristically. */
[[nodiscard]] std::vector<DenseRegion> observe_dense_regions(const source_dag::SourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues,
                                                             std::map<std::string, size_t> & issues);

[[nodiscard]] std::vector<AttentionMetadataRegion> observe_attention_metadata_regions(const source_dag::SourceDagIndex & source,
                                                                                      const simulation::detail::CpuTaskQueues & queues);

/** A source call prepared once for target planning; no costs or target data.
 * phase_index refers to the accompanying immutable phase observation audit.
 */
struct AttentionCall {
    AttentionRegion region;
    PhaseCallPosition position;
    adapters::sglang::AscendAttentionConditions conditions;
    adapters::sglang::AttentionBatchShape batch;
    adapters::sglang::AttentionCacheWrite cache_write;
    std::optional<adapters::sglang::AttentionEagerPadding> padding;
};

struct AttentionCallAudit {
    std::vector<AttentionCall> calls;
    std::map<std::string, size_t> issues;
};

/** Resolve packed-row K/V layouts from direct metadata or the same source
 * forward/layer's proven Qwen3 producers. Does not inspect device durations. */
void resolve_attention_cache_layouts(const source_dag::SourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues,
                                     const PhaseCallPositions & positions, AttentionCallAudit & attention);

struct DenseCall {
    DenseRegion region;
    PhaseCallPosition position;
    size_t attention_call_index;
    std::string role;
    uint64_t layer_index;
    uint64_t rows_per_token;
};

struct DenseCallAudit {
    std::vector<DenseCall> calls;
    std::map<std::string, size_t> issues;
};

/** Qwen3 dense decoder computation with an explicit module-role descriptor.
 * Join to the same source forward/layer's attention descriptor to establish
 * token layout, not by dimension coincidence or timestamp proximity. */
[[nodiscard]] DenseCallAudit observe_dense_calls(const source_dag::SourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues,
                                                 const PhaseCallPositions & positions, const AttentionCallAudit & attention);

struct AttentionMetadataCall {
    AttentionMetadataRegion region;
    PhaseCallPosition position;
    adapters::sglang::AttentionMetadataInputs inputs;
    bool complete_prefill_context;
};

struct AttentionMetadataAudit {
    std::vector<AttentionMetadataCall> calls;
    std::map<std::string, size_t> issues;
};

[[nodiscard]] AttentionMetadataAudit observe_attention_metadata_calls(const source_dag::SourceDagIndex & source,
                                                                      const simulation::detail::CpuTaskQueues & queues,
                                                                      const modules::hicache::HiCachePhaseObservationAudit & phases,
                                                                      const PhaseCallPositions & positions);

[[nodiscard]] AttentionCallAudit observe_attention_calls(const source_dag::SourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues,
                                                         const modules::hicache::HiCachePhaseObservationAudit & phases, const PhaseCallPositions & positions);

[[nodiscard]] std::vector<AllocatorRegion> observe_allocator_regions(const source_dag::SourceDagIndex & source,
                                                                     const simulation::detail::CpuTaskQueues & queues);
// Deployment ABI only. Missing/conflicting dtype or debug metadata is not a
// supported default; target page counts and execution branches are not read.
[[nodiscard]] std::map<std::string, adapters::sglang::AscendAllocatorInputs> observe_allocator_inputs(const core::DagGraph & graph);
// Method-call metadata: extend and decode can legitimately use different
// last_loc dtypes on the same process. Never merge those into one ABI guess.
[[nodiscard]] std::optional<adapters::sglang::AscendAllocatorInputs> observed_allocator_inputs(const core::TraceEvent & event);
/** A successful inner call's own observed geometry and queue transition.
 * Request/iteration identity is supplied by the enclosing workload boundary. */
[[nodiscard]] std::optional<modules::hicache::model::HiCacheAllocatorWorkItem> observed_allocator_work(const core::TraceEvent & event);
// Descriptor identity is local to pid/phase; unresolved references remain
// absent. Sparse recordings establish only the calls actually observed.
[[nodiscard]] std::vector<AttentionRegion> observe_attention_regions(const source_dag::SourceDagIndex & source,
                                                                     const simulation::detail::CpuTaskQueues & queues);

/** Recognize the native queued submission implementation; unknown task
 * names remain unknown and cannot be pooled into reduction CPU overhead. */
[[nodiscard]] std::optional<structure::hicache::CollectiveTaskKind> collective_task_kind(std::string_view submission_name);

/** One rank's observed ordinary all-reduce. Shape is a packed token-major
 * matrix, not a target shape. Phase/layer ownership and all peer descriptors
 * have been corroborated; no communication duration is a service estimate. */
struct DeviceCollectiveCall {
    source_dag::SubmittedCallRegion region;
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
[[nodiscard]] std::optional<std::vector<CollectiveCallerInterval>> observe_collective_caller_intervals(const source_dag::SourceDagIndex & source,
                                                                                                       const simulation::detail::CpuTaskQueues & queues,
                                                                                                       const DeviceCollectiveCall & call,
                                                                                                       std::span<const source_dag::QueuedCallTask> tasks);

/** Source-only attribution for the current ordinary Qwen3 TP path. Unknown
 * layouts, missing peers and ambiguous submission ownership are issues, not
 * zero work or inferred peer calls. This does not prove target dispatch.
 * Descriptor agreement is checked as typed work, not by serializing a report.
 * The source index and queues describe the same immutable graph throughout this call;
 * positions must have been prepared from the accompanying phase audit. */
[[nodiscard]] DeviceCollectiveAudit observe_device_collectives(const source_dag::SourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues,
                                                               const modules::hicache::HiCachePhaseObservationAudit & phases,
                                                               const PhaseCallPositions & positions);


/** Immutable source calls joined to the same phase observation audit.
 * Dense call indices address attention.calls; phase indices address that audit.
 * Observation issues retain their family and are combined only on request.
 * No target work, costs or graph mutation are produced here.
 */
struct PhaseCallAudit {
    AttentionCallAudit attention;
    AttentionMetadataAudit metadata;
    DenseCallAudit dense;
    DeviceCollectiveAudit collectives;

    [[nodiscard]] std::map<std::string, size_t> issues() const;
};

/** Observe all phase families before source replacement.
 * The index and queues must describe the same immutable source graph.
 */
[[nodiscard]] PhaseCallAudit observe_phase_calls(const source_dag::SourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues,
                                                 const modules::hicache::HiCachePhaseObservationAudit & phases);

} // namespace markov::trace_graph::adapters::sglang
