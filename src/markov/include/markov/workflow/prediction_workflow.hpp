#pragma once

#include "markov/workflow/project.hpp"

#include <filesystem>
#include <span>

#include <nlohmann/json_fwd.hpp>

namespace markov {

class TemplateAdapter;

class PredictionWorkflow {
public:
    explicit PredictionWorkflow(std::span<const TemplateAdapter * const> adapters) : adapters_(adapters) {}

    nlohmann::json run(const Project & project, nlohmann::json collection_plan, const std::filesystem::path & catalog_directory = {}) const;

private:
    std::span<const TemplateAdapter * const> adapters_; // Owned by the entry point for the full run.
};

} // namespace markov
