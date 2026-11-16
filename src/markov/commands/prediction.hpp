#pragma once

#include "markov/adapters/prediction.hpp"

namespace markov::commands {

nlohmann::json prediction_report(const adapters::PreparedPrediction & input);
nlohmann::json prediction_report(const adapters::sglang::PreparedPrediction & input);
nlohmann::json prediction_report(const adapters::ktransformers::PreparedPrediction & input);

} // namespace markov::commands
