#pragma once
// Independent CPU/control observation export; not target program import.
#include "markov/trace_graph/core/dag_graph.hpp"
#include <cstdint>
#include <nlohmann/json_fwd.hpp>

namespace markov::trace_graph::modules::hicache::runtime {

/** Export available write and eviction measurements; CPU corrections must already
 * be attached to the source graph. Empty rows mean no completed operation was
 * observed, not zero service cost. Release-only extraction selects ordinary
 * releases. Mutates only the supplied working graph; does not consume target data. */
[[nodiscard]] nlohmann::json export_write_calibration(core::DagGraph & graph, uint64_t begin, uint64_t end, bool release_only = false);

} // namespace markov::trace_graph::modules::hicache::runtime
