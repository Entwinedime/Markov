/**
 * @file
 * @brief Internal in-process joining primitives for manifest trace channels.
 */
#pragma once

#include "markov/trace_graph/core/trace_event.hpp"
#include "markov/trace_graph/io/trace_manifest_input.hpp"

#include <vector>

namespace markov::trace_graph::io::detail {

/**
 * @brief Aligns custom wrapper/probe events to profiler time and appends them.
 *
 * `profiler_events` owns the destination timeline. `custom_events` is consumed by
 * value so matched metadata can be moved without retaining a second trace copy.
 */
void join_custom_trace(std::vector<core::TraceEvent> & profiler_events, std::vector<core::TraceEvent> custom_events, const ManifestTraceInputOptions & options);

/** @brief Removes metadata after channel joining; `DagBuilder` owns ordering. */
void retain_duration_events(std::vector<core::TraceEvent> & events);

} // namespace markov::trace_graph::io::detail
