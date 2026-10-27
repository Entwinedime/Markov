#pragma once

#include <nlohmann/json.hpp>

namespace markov::adapters {
struct PreparedSource;
}

namespace markov::commands {

nlohmann::json source_report(const adapters::PreparedSource & source);

} // namespace markov::commands
