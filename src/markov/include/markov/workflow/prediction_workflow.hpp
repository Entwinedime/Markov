#pragma once

#include "markov/contracts/project.hpp"

#include <nlohmann/json_fwd.hpp>

namespace markov {

class PredictionWorkflow {
public:
    nlohmann::json run(const Project & project) const;
};

} // namespace markov
