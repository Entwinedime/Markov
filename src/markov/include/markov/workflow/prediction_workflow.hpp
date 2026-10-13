#pragma once

#include "markov/contracts/project.hpp"

#include <span>

#include <nlohmann/json_fwd.hpp>

namespace markov {

class TemplateAdapter;

class PredictionWorkflow {
public:
    explicit PredictionWorkflow(std::span<const TemplateAdapter * const> adapters) : adapters_(adapters) {}

    nlohmann::json run(const Project & project, nlohmann::json collection_plan) const;

private:
    std::span<const TemplateAdapter * const> adapters_; // Owned by the entry point for the full run.
};

} // namespace markov
