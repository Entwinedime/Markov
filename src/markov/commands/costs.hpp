#pragma once

#include "markov/adapters/costs.hpp"

#include <nlohmann/json.hpp>

namespace markov::commands {

// Work coverage is independent of source replay and target evaluation. A ready
// response is an operation cost, not evidence of structural or E2E accuracy.
nlohmann::json cost_report(const adapters::CostQueries & queries, const adapters::SelectedCosts & selected);

} // namespace markov::commands
