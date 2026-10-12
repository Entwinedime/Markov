#pragma once

#include "markov/trace_graph/core/dag_graph.hpp"
#include "markov/trace_graph/modules/hicache/phase_observation.hpp"

#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>

namespace markov::trace_graph::source_dag {

/** Stable original record and interval identity; synthetic work has no measurement. */
[[nodiscard]] std::optional<std::string> measurement_position(const core::TraceEvent & event);

/** Preserve observed Torch input dimensions and source submission links.
 * Unsupported encodings retain raw text with null parsed dimensions.
 * Strict innermost envelopes establish source-local ownership; ties and
 * non-nested candidates remain ambiguous, never resolved by kernel name.
 * This source evidence neither emits costs nor generates target geometry. */
[[nodiscard]] nlohmann::json operator_geometry_observations(const core::DagGraph & graph, const modules::hicache::HiCachePhaseObservationAudit & phases);

/** One group observation after the last measured input frontier, ending at
 * the last rank's communication completion. This is a completion interval,
 * not pure transport service. Requires raw, complete predecessor intervals
 * consistent with device starts in the source graph's common clock. Caller
 * and worker measurements remain separate per-rank CPU scopes and use only
 * this capture's correction; raw HCCL duration is never a device sample. */
[[nodiscard]] nlohmann::json collective_operator_observations(const core::DagGraph & graph, const modules::hicache::HiCachePhaseObservationAudit & phases,
                                                              bool cpu_service_applied, nlohmann::json * collective_diagnostics = nullptr);

/** Attention, metadata and Linear samples with observed operator/submission ownership.
 * Missing or ambiguous native lowering is omitted, not filled from call wall time.
 * Device samples require unmodified raw durations. Their absence does not
 * invalidate independently measurable caller/worker scopes.
 */
[[nodiscard]] nlohmann::json phase_tensor_operator_observations(const core::DagGraph & graph, const modules::hicache::HiCachePhaseObservationAudit & phases,
                                                                bool cpu_service_applied);

/** Extend/Decode allocator measurements using observed request lengths,
 * queue transitions and matched Torch/Triton calls. Missing geometry or
 * timing scopes are not imputed; cold launch preparation is not counted twice. */
[[nodiscard]] nlohmann::json allocator_operator_observations(const core::DagGraph & graph, bool cpu_service_applied);

/** Operator-level observations on source coordinates, never target projections.
 * Profiled CPU and corrected CPU are different timing scopes. Device values
 * represent service, not the enclosing phase wall time or resource queue wait.
 * Optional collective diagnostics reuse the same source attribution as samples.
 */
[[nodiscard]] nlohmann::json phase_operator_observations(const core::DagGraph & graph, const modules::hicache::HiCachePhaseObservationAudit & phases,
                                                         bool cpu_service_applied, nlohmann::json * collective_diagnostics = nullptr);

/** Whole nonblocking CPU tails after MIN, indexed by the observed queue delta,
 * plus empty blocking write entry/return and separately observed ack bodies.
 * Ack bodies are nested alternatives, not extra cost on top of whole tails.
 * CPU service and residual intervals are separate; unsupported batching or
 * incomplete interval ownership is not admitted as a numeric measurement.
 * Synchronization needs proof that its device predecessor completed before
 * API entry; graph-normalized sync costs are never measured CPU samples.
 */
[[nodiscard]] nlohmann::json confirmation_operator_observations(const core::DagGraph & graph, bool cpu_service_applied);

/** CPU preparation before the first allocator attempt and cleanup after the
 * final failed attempt. Neither interval includes allocator service or eviction. */
[[nodiscard]] nlohmann::json load_admission_operator_observations(const core::DagGraph & graph, bool cpu_service_applied);

/** Scheduler prefix concatenation after init_load_back, separate from the
 * admitted allocator/clone interval. Only the supported op-api layout is admitted. */
[[nodiscard]] nlohmann::json prefix_concat_operator_observations(const core::DagGraph & graph, bool cpu_service_applied);

/** Per-call active submissions/returns and inactive layer waits. No per-rank
 * averages or observed dependency templates are used as operator measurements. */
[[nodiscard]] nlohmann::json layer_wait_operator_observations(const core::DagGraph & graph, bool cpu_service_applied);

} // namespace markov::trace_graph::source_dag
