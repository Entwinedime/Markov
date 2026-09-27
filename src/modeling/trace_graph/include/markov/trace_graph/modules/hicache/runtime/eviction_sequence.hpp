#pragma once
#include "markov/trace_graph/modules/hicache/model/state.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_host.hpp"

namespace markov::trace_graph::modules::hicache::runtime {

/** Logical target work only. Templates supply costs later; a victim index
 * refers to target state, never to a donor's object or completion event. */
struct HiCacheEvictionStep {
    HiCacheEvictionRegion::Kind kind;
    HiCacheEvictionRegion::ControlPhase control_phase = HiCacheEvictionRegion::ControlPhase::None;
    std::optional<size_t> victim;
    std::optional<HiCacheEvictionRegion::Kind> preceding_kind;
    size_t heap_size = 0;
};

[[nodiscard]] std::vector<HiCacheEvictionStep> eviction_sequence(const model::HiCacheEvictionWork & work, bool write_back);
[[nodiscard]] double locked_candidate_cost(size_t heap_size, double fixed_us, double log2_heap_us);

struct EvictionControlCostSample {
    HiCacheEvictionRegion::ControlPhase phase;
    double cpu_us = 0, residual_us = 0;
    bool independent = false;
};
struct EvictionControlCost {
    double cpu_us = 0, residual_us = 0;
    size_t samples = 0;
    bool phase_extrapolated = false;
    bool independent = false;
};
/** Cost only: phase mean, or explicitly pooled base-control extrapolation.
 * No source topology, predecessor kind, victim or completion identity enters
 * this estimate. Residual service is retained separately, not called CPU work.
 */
[[nodiscard]] std::optional<EvictionControlCost> estimate_eviction_control_cost(
    std::span<const EvictionControlCostSample> samples, HiCacheEvictionRegion::ControlPhase phase);

} // namespace markov::trace_graph::modules::hicache::runtime
