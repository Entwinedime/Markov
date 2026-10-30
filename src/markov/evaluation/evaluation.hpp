#pragma once

#include "markov/adapters/replay.hpp"

#include <filesystem>
#include <nlohmann/json.hpp>

namespace markov::evaluation {

struct PreparedEvaluation {
    adapters::PredictedTrajectory prediction;
    trace_graph::source::ProfileManifest base;
    adapters::PreparedSource target;
};

PreparedEvaluation prepare(const std::filesystem::path & input);
nlohmann::json report(const PreparedEvaluation & input);

} // namespace markov::evaluation
