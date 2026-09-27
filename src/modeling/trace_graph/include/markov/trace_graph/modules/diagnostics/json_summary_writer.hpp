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

#include <nlohmann/json_fwd.hpp>

namespace markov::trace_graph::modules::diagnostics {

/** @brief Builds one module's JSON value; the CLI serializes the final document. */
[[nodiscard]] nlohmann::json module_summary_json(const SimulationModule & module);

} // namespace markov::trace_graph::modules::diagnostics
