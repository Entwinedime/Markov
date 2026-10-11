#pragma once

#include "markov/trace_graph/core/dag_graph.hpp"
#include "markov/trace_graph/modules/hicache/patch/source_dag_index.hpp"
#include "markov/trace_graph/simulation/cpu_task_queues.hpp"
#include "markov/trace_graph/source_dag/decode_allocation.hpp"
#include "markov/trace_graph/structure/hicache/allocator.hpp"
#include "markov/trace_graph/structure/hicache/attention.hpp"
#include "markov/trace_graph/structure/phase_shape.hpp"
#include "markov/trace_graph/structure/tensor_program.hpp"

#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace markov::trace_graph::source_dag {

/** Ascend task-label suffix for a supported explicit native implementation.
 * Labels corroborate operator/engine identity, not dtype or tensor geometry. */
[[nodiscard]] std::optional<std::string_view> native_launch_label(std::string_view implementation);

/** Zero-service queue placeholders strictly between owned device nodes.
 * Only a unique same-lane Stream chain without any other active dependency
 * is included. Caller ownership is not inferred from temporal proximity. */
[[nodiscard]] std::set<size_t> interior_stream_placeholders(const modules::hicache::patch::HiCacheSourceDagIndex & source, const std::set<size_t> & owned);

/** Exact call envelope and its submission closure in the immutable graph.
 * Node ids are source-local. Stream/queue successors are not ownership;
 * sequential members of the same explicitly submitted CPU task are included.
 * external dependencies must still be reattached by the replacement planner.
 */
struct SubmittedCallRegion {
    size_t observation;
    modules::hicache::patch::HiCacheTimingIntervalOwnership caller;
    std::vector<size_t> submitted_nodes;
    std::vector<size_t> incoming_edges, outgoing_edges;
};

[[nodiscard]] SubmittedCallRegion observe_submitted_call_region(const modules::hicache::patch::HiCacheSourceDagIndex & source,
                                                                const simulation::detail::CpuTaskQueues & queues, size_t observation);

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
[[nodiscard]] std::optional<std::vector<QueuedCallTask>> observe_queued_call_tasks(const modules::hicache::patch::HiCacheSourceDagIndex & source,
                                                                                   const simulation::detail::CpuTaskQueues & queues,
                                                                                   const SubmittedCallRegion & region);

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
 * this establishes semantics, not zero execution cost. */
[[nodiscard]] PhaseCallPositions phase_call_positions(const modules::hicache::HiCachePhaseObservationAudit & phases);
[[nodiscard]] std::optional<PhaseCallPosition> call_phase_position(const SubmittedCallRegion & region, const PhaseCallPositions & positions);

/** Decode ordinary metadata inputs without measuring costs. Capture, SWA,
 * MLA, speculative/multi-request work and incomplete evidence are unavailable.
 * Context is the framework's recorded scalar, not total prompt length or
 * a value inferred from page-table width. No target data is consumed. */
[[nodiscard]] std::optional<structure::hicache::AttentionMetadataInputs> observed_attention_metadata_inputs(const core::TraceEvent & descriptor);

/** Proven caller-side synchronous readback: one stream wait followed by one
 * synchronous memcpy, with no worker submission. device_completion identifies
 * the waited producer; completion is the final owned caller node. This proves
 * structure only: cross-clock measurements are checked by cost extraction. */
struct SynchronousReadback {
    size_t synchronization, completion, device_completion;
};
[[nodiscard]] std::optional<SynchronousReadback> observe_synchronous_readback(const modules::hicache::patch::HiCacheSourceDagIndex & source,
                                                                              const SubmittedCallRegion & region);

struct AllocatorRegion {
    SubmittedCallRegion work;
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

[[nodiscard]] std::vector<ObservedAllocatorProgram> observe_allocator_programs(const modules::hicache::patch::HiCacheSourceDagIndex & source,
                                                                               const simulation::detail::CpuTaskQueues & queues);

struct AttentionDescriptorPosition {
    bool prelude;
    size_t observation;
};

struct AttentionRegion {
    SubmittedCallRegion work;
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
    SubmittedCallRegion work;
    std::vector<size_t> operator_observations;
};

struct DenseRegion {
    SubmittedCallRegion work;
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
[[nodiscard]] std::vector<DenseRegion> observe_dense_regions(const modules::hicache::patch::HiCacheSourceDagIndex & source,
                                                             const simulation::detail::CpuTaskQueues & queues, std::map<std::string, size_t> & issues);

[[nodiscard]] std::vector<AttentionMetadataRegion> observe_attention_metadata_regions(const modules::hicache::patch::HiCacheSourceDagIndex & source,
                                                                                      const simulation::detail::CpuTaskQueues & queues);

/** Match semantic tensor operations to original Torch call envelopes.
 * Keys are indices in program; values address graph.runtime_observations().
 * Missing or non-unique lowering is omitted, never paired by proximity.
 */
[[nodiscard]] std::map<size_t, size_t> match_tensor_operators(const modules::hicache::patch::HiCacheSourceDagIndex & source,
                                                              const simulation::detail::CpuTaskQueues & queues, std::span<const size_t> operator_observations,
                                                              std::span<const structure::TensorOperation> program);

/** Pair complete operator families in trace order. expected maps program
 * positions to framework Torch schemas; nested extra calls make that family
 * ambiguous instead of allowing a timing-nearest or partial correspondence. */
[[nodiscard]] std::map<size_t, size_t> match_operator_envelopes(const core::DagGraph & graph, std::span<const size_t> observations,
                                                                const std::map<size_t, std::string> & expected);

/** Device-backed families or proven synchronous readbacks only: CPU-only
 * casts/arithmetic sharing a Torch schema are not target device operations. */
[[nodiscard]] std::map<size_t, size_t> match_submitted_operator_envelopes(const modules::hicache::patch::HiCacheSourceDagIndex & source,
                                                                          const simulation::detail::CpuTaskQueues & queues,
                                                                          std::span<const size_t> observations, const std::map<size_t, std::string> & expected);

struct DeviceChain {
    size_t first, last;
};

/** A single observed stream chain without interleaved foreign work or event
 * waits. This establishes boundaries, not a cost or predicted lowering. */
[[nodiscard]] std::optional<DeviceChain> observe_serial_device_chain(const modules::hicache::patch::HiCacheSourceDagIndex & source,
                                                                     const std::set<size_t> & devices);

/** A source call prepared once for target planning; no costs or target data.
 * phase_index refers to the accompanying immutable phase observation audit.
 */
struct AttentionCall {
    AttentionRegion region;
    size_t phase_index;
    std::optional<uint64_t> decode_iteration;
    structure::hicache::AscendAttentionConditions conditions;
    structure::hicache::AttentionBatchShape batch;
    structure::hicache::AttentionCacheWrite cache_write;
    std::optional<structure::hicache::AttentionEagerPadding> padding;
};

struct AttentionCallAudit {
    std::vector<AttentionCall> calls;
    std::map<std::string, size_t> issues;
};

/** Resolve packed-row K/V layouts from direct metadata or the same source
 * forward/layer's proven Qwen3 producers. Does not inspect device durations. */
void resolve_attention_cache_layouts(const modules::hicache::patch::HiCacheSourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues,
                                     const modules::hicache::HiCachePhaseObservationAudit & phases, AttentionCallAudit & attention);

struct DenseCall {
    DenseRegion region;
    size_t phase_index, attention_call_index;
    std::optional<uint64_t> decode_iteration;
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
[[nodiscard]] DenseCallAudit observe_dense_calls(const modules::hicache::patch::HiCacheSourceDagIndex & source,
                                                 const simulation::detail::CpuTaskQueues & queues,
                                                 const modules::hicache::HiCachePhaseObservationAudit & phases, const AttentionCallAudit & attention);

struct AttentionMetadataCall {
    AttentionMetadataRegion region;
    size_t phase_index;
    std::optional<uint64_t> decode_iteration;
    structure::hicache::AttentionMetadataInputs inputs;
    bool complete_prefill_context;
};

struct AttentionMetadataAudit {
    std::vector<AttentionMetadataCall> calls;
    std::map<std::string, size_t> issues;
};

[[nodiscard]] AttentionMetadataAudit observe_attention_metadata_calls(const modules::hicache::patch::HiCacheSourceDagIndex & source,
                                                                      const simulation::detail::CpuTaskQueues & queues,
                                                                      const modules::hicache::HiCachePhaseObservationAudit & phases);

[[nodiscard]] AttentionCallAudit observe_attention_calls(const modules::hicache::patch::HiCacheSourceDagIndex & source,
                                                         const simulation::detail::CpuTaskQueues & queues,
                                                         const modules::hicache::HiCachePhaseObservationAudit & phases);

[[nodiscard]] std::vector<AllocatorRegion> observe_allocator_regions(const modules::hicache::patch::HiCacheSourceDagIndex & source,
                                                                     const simulation::detail::CpuTaskQueues & queues);
// Deployment ABI only. Missing/conflicting dtype or debug metadata is not a
// supported default; target page counts and execution branches are not read.
[[nodiscard]] std::map<std::string, structure::hicache::AscendAllocatorInputs> observe_allocator_inputs(const core::DagGraph & graph);
// Method-call metadata: extend and decode can legitimately use different
// last_loc dtypes on the same process. Never merge those into one ABI guess.
[[nodiscard]] std::optional<structure::hicache::AscendAllocatorInputs> observed_allocator_inputs(const core::TraceEvent & event);
/** A successful inner call's own observed geometry and queue transition.
 * Request/iteration identity is supplied by the enclosing workload boundary. */
[[nodiscard]] std::optional<modules::hicache::model::HiCacheAllocatorWorkItem> observed_allocator_work(const core::TraceEvent & event);
// Descriptor identity is local to pid/phase; unresolved references remain
// absent. Sparse recordings establish only the calls actually observed.
[[nodiscard]] std::vector<AttentionRegion> observe_attention_regions(const modules::hicache::patch::HiCacheSourceDagIndex & source,
                                                                     const simulation::detail::CpuTaskQueues & queues);

// Missing branch metadata is not false/default. This reads only an explicit
// descriptor record; references must first be resolved within the source trace.
[[nodiscard]] std::optional<structure::hicache::AscendAttentionConditions> attention_conditions(const core::TraceEvent & descriptor);

// Only explicitly recorded per-request lengths establish the observed batch.
// Historical descriptors and decode calls without lengths remain unavailable.
[[nodiscard]] std::optional<structure::hicache::AttentionBatchShape> attention_batch_shape(const core::TraceEvent & descriptor,
                                                                                           std::optional<uint64_t> observed_decode_context = std::nullopt);

// Correspondence is established by the call's submitted source operators,
// not nearby timestamps or the first descriptor's original decode round.
[[nodiscard]] std::optional<DecodeOperationContext> attention_decode_context(const AttentionRegion & region, const DecodeOperatorContexts & contexts);

[[nodiscard]] std::optional<structure::hicache::AttentionCacheWrite> attention_cache_write(const core::TraceEvent & descriptor);

// Observed conditions only. A target batch must establish its own TBO choice.
[[nodiscard]] std::optional<structure::hicache::AttentionEagerPadding> attention_eager_padding(const core::TraceEvent & descriptor);

} // namespace markov::trace_graph::source_dag
