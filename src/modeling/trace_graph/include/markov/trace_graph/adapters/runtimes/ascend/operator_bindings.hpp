#pragma once

#include "markov/trace_graph/source_dag/call_regions.hpp"
#include "markov/trace_graph/structure/tensor_program.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace markov::trace_graph::adapters::ascend {

/** Ascend task-label suffix for a supported explicit native implementation.
 * Labels corroborate operator/engine identity, not dtype or tensor geometry. */
[[nodiscard]] std::optional<std::string_view> native_launch_label(std::string_view implementation);

/** Proven caller-side synchronous readback: one stream wait followed by one
 * synchronous memcpy, with no worker submission. device_completion identifies
 * the waited producer; completion is the final owned caller node. This proves
 * structure only: cross-clock measurements are checked by cost extraction. */
struct SynchronousReadback {
    size_t synchronization, completion, device_completion;
};
[[nodiscard]] std::optional<SynchronousReadback> observe_synchronous_readback(const source_dag::SourceDagIndex & source,
                                                                              const source_dag::SubmittedCallRegion & region);

/** Match semantic tensor operations to original Torch call envelopes.
 * Keys are indices in program; values address graph.runtime_observations().
 * Missing or non-unique lowering is omitted, never paired by proximity.
 */
[[nodiscard]] std::map<size_t, size_t> match_tensor_operators(const source_dag::SourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues,
                                                              std::span<const size_t> operator_observations,
                                                              std::span<const structure::TensorOperation> program);

/** Device-backed families or proven synchronous readbacks only: CPU-only
 * casts/arithmetic sharing a Torch schema are not target device operations. */
[[nodiscard]] std::map<size_t, size_t> match_submitted_operator_envelopes(const source_dag::SourceDagIndex & source,
                                                                          const simulation::detail::CpuTaskQueues & queues,
                                                                          std::span<const size_t> observations, const std::map<size_t, std::string> & expected);

} // namespace markov::trace_graph::adapters::ascend
