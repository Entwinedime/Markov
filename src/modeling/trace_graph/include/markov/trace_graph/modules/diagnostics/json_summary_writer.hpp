/**
 * @file
 * @brief Diagnostics JSON boundary for SimulationModule summaries.
 *
 * Modules expose typed summaries. This writer performs dynamic dispatch at the CLI boundary,
 * keeping JSON virtual functions out of the business interface. Unknown modules receive a
 * stable unsupported record instead of silently disappearing.
 */
#pragma once

#include "markov/trace_graph/modules/module.hpp"

#include <string>

namespace markov::trace_graph::modules::diagnostics {

/** @brief Serializes the summary of one executed module to JSON text. */
[[nodiscard]] std::string module_summary_json(const SimulationModule & module);

} // namespace markov::trace_graph::modules::diagnostics
