#pragma once

#include "markov/adapters/ktransformers/prediction.hpp"
#include "markov/adapters/sglang/prediction.hpp"

#include <filesystem>
#include <variant>

namespace markov::adapters {

using PreparedPrediction = std::variant<sglang::PreparedPrediction, ktransformers::PreparedPrediction>;

PreparedPrediction prepare_prediction(const std::filesystem::path & path);

} // namespace markov::adapters
