#include "markov/commands/prediction.hpp"

#include <variant>

namespace markov::commands {

nlohmann::json prediction_report(const adapters::PreparedPrediction & input) {
    return std::visit([](const auto & prepared) { return prediction_report(prepared); }, input);
}

} // namespace markov::commands
