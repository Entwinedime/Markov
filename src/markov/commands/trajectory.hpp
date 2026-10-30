#pragma once

#include "markov/adapters/replay.hpp"
#include "markov/trace_graph/simulation/program_execution.hpp"

#include <nlohmann/json.hpp>

namespace markov::commands {

// Diagnostics are generated only when recording was explicitly requested.
nlohmann::json generated_graph_report(const trace_graph::simulation::ProgramResult & result);
nlohmann::json trajectory_replay_report(const adapters::PreparedReplay & input);

} // namespace markov::commands
