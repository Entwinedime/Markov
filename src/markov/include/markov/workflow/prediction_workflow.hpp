#pragma once

#include "markov/workflow/project.hpp"

#include <filesystem>
#include <span>

#include <nlohmann/json_fwd.hpp>

namespace markov {

class TemplateAdapter;

struct PredictionOutputs {
    std::filesystem::path requirements;
    std::filesystem::path requests;
};

class PredictionWorkflow {
public:
    explicit PredictionWorkflow(std::span<const TemplateAdapter * const> adapters) : adapters_(adapters) {}

    nlohmann::json run(const Project & project, nlohmann::json collection_plan, const PredictionOutputs & outputs) const;

private:
    std::span<const TemplateAdapter * const> adapters_; // Owned by the entry point for the full run.
};

} // namespace markov
