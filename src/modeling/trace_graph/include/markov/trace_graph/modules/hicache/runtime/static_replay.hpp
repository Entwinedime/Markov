#pragma once

#include "markov/trace_graph/modules/hicache/model/state.hpp"

namespace markov::trace_graph::modules::hicache::runtime {

/** Explicit static diagnostic composition; does not mutate the DAG.
 * An empty clock retains the source-timed approximation. A supplied clock
 * does not replace the live execution loop or its publication/cancel events.
 * I/O completion is composed here; optional phase costs are bound by the
 * subsequent diagnostic patch, never stored in the structural result. */
[[nodiscard]] model::HiCacheModelResult replay_hicache_static(core::DagGraph & graph, const frontend::HiCacheConfig & config,
                                                              const model::HiCacheFactClock & execution_clock = {});

} // namespace markov::trace_graph::modules::hicache::runtime
