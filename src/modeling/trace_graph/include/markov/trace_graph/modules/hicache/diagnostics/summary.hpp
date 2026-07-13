/**
 * @file
 * @brief Diagnostics JSON writer for a HiCache model summary.
 *
 * This layer defines no model state. It serializes `model::HiCacheSummary` for workflow
 * validation, and the state-machine core must not depend on this header.
 */
#pragma once

#include "markov/trace_graph/modules/hicache/model/effect_decision.hpp"
#include "markov/trace_graph/modules/hicache/model/summary.hpp"

#include <string>

namespace markov::trace_graph::modules::hicache::diagnostics {

/**
 * @brief Serializes one structured HiCache model result to JSON text.
 *
 * The function is diagnostics-only and never mutates or re-derives the summary.
 */
[[nodiscard]] std::string summary_json(const model::HiCacheSummary & summary, const model::HiCacheEffectDecisionLedger & effect_decisions);

} // namespace markov::trace_graph::modules::hicache::diagnostics
