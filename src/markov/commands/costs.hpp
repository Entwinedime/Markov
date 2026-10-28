#pragma once

#include "markov/adapters/costs.hpp"

#include <nlohmann/json.hpp>
#include <string>

namespace markov::adapters {
struct PreparedSource;
}

namespace markov::commands {

const std::string & cost_status_name(trace_graph::cost::Status status);
const std::string & cost_coverage_name(trace_graph::cost::Coverage coverage);
nlohmann::json work_json(const trace_graph::planning::Work & work);

// Work coverage is independent of source replay and target evaluation. A ready
// response is an operation cost, not evidence of structural or E2E accuracy.
nlohmann::json cost_report(const adapters::CostQueries & queries, const adapters::SelectedCosts & selected);
nlohmann::json source_cpu_cost_report(const adapters::PreparedSource & source);

} // namespace markov::commands
