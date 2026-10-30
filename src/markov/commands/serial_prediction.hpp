#pragma once

#include "markov/adapters/prediction.hpp"

namespace markov::commands {

nlohmann::json serial_prediction_report(const adapters::PreparedPrediction & input);

} // namespace markov::commands
