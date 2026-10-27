/**
 * @file
 * @brief Debug-only TraceGraph CLI diagnostics boundary.
 */
#pragma once

#ifdef DEBUG
#include "markov/trace_graph/modules/module.hpp"

#include <memory>
#include <string>
#include <vector>

namespace markov::trace_graph::cli {

/**
 * @brief Writes summaries exposed by modules after they have executed.
 *
 * The writer is available only in diagnostics builds. Release builds neither
 * expose module summaries nor link their JSON serializers.
 */
void write_module_summary(const std::string & filename, const std::vector<std::unique_ptr<modules::SimulationModule>> & modules);

} // namespace markov::trace_graph::cli
#endif
