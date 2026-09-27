/**
 * @file
 * @brief Compact diagnostics JSON writer for the predicted HiCache effect plan.
 */
#pragma once

#include "markov/trace_graph/modules/hicache/model/effect_decision.hpp"

#include <nlohmann/json_fwd.hpp>

namespace markov::trace_graph::modules::hicache::diagnostics {

/**
 * @brief Builds the JSON value consumed by structure validation, without encoding text.
 */
[[nodiscard]] nlohmann::json summary_json(const model::HiCacheEffectDecisionLedger & effect_plan);

} // namespace markov::trace_graph::modules::hicache::diagnostics
