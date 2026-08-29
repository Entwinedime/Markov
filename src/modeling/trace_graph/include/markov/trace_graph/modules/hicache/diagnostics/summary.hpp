/**
 * @file
 * @brief Compact diagnostics JSON writer for the predicted HiCache effect plan.
 */
#pragma once

#include "markov/trace_graph/modules/hicache/model/effect_decision.hpp"

#include <string>

namespace markov::trace_graph::modules::hicache::diagnostics {

/**
 * @brief Serializes only fields consumed by current structure validation.
 */
[[nodiscard]] std::string summary_json(const model::HiCacheEffectDecisionLedger & effect_plan);

} // namespace markov::trace_graph::modules::hicache::diagnostics
