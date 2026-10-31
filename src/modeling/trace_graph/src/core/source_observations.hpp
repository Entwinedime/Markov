#pragma once

#include "markov/trace_graph/core/trace_event.hpp"

#include <cstdint>
#include <vector>

namespace markov::trace_graph::core {

/** @brief Source-observed wallclock spans excluded from control-only replay. */
enum class DagControlExclusionKind : std::uint8_t { PrefillDecode, ProfilingSnapshot };

/** @brief One absolute source-trace interval removed only by control-only replay. */
struct DagControlExclusionInterval {
    int gpu_id = 0;
    uint64_t start_us = 0;
    uint64_t end_us = 0;
    DagControlExclusionKind kind = DagControlExclusionKind::PrefillDecode;
};

/** @brief One runtime phase marker retained outside executable leaf topology. */
struct DagPhaseMarkerEvent {
    int gpu_id = 0;
    TraceEvent event;
};

/** Source-only facts, envelopes and context; no executable nodes or costs.
 * Graph copies share this immutable input. A merger takes ownership before writing.
 */
struct SourceObservations {
    std::vector<TraceEvent> hicache_fact_events, runtime_observations, context_events, prelude_context_events, tail_context_events;
    std::vector<DagControlExclusionInterval> control_exclusion_intervals;
    std::vector<DagPhaseMarkerEvent> phase_marker_events;
};

} // namespace markov::trace_graph::core
