#pragma once

#include "markov/adapters/replay.hpp"

namespace markov::commands {

nlohmann::json replay_report(const adapters::PreparedReplay & input);

} // namespace markov::commands
