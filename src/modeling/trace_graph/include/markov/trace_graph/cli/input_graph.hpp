#pragma once

#include "markov/trace_graph/core/dag_graph.hpp"
#include "markov/trace_graph/io/trace_manifest_input.hpp"

#include <cstddef>
#include <vector>

namespace markov::trace_graph::cli {

/** Consume joined manifest inputs, preserving their logical IDs and context.
 * The positive thread budget covers concurrent inputs and each input's builder.
 * No target data or model transformations are involved.
 */
core::DagGraph build_input_graph(std::vector<io::ManifestTraceInput> inputs, size_t thread_budget);

} // namespace markov::trace_graph::cli
