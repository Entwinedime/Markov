#pragma once

#include "markov/workflow/project.hpp"

#include <nlohmann/json_fwd.hpp>

#include <filesystem>
#include <span>

namespace markov {

class TemplateAdapter;
class CollectionAdapter;

class StructureComparisonWorkflow {
public:
    StructureComparisonWorkflow(std::span<const TemplateAdapter * const> adapters, std::span<const CollectionAdapter * const> collection_adapters)
        : adapters_(adapters),
          collection_adapters_(collection_adapters) {}

    nlohmann::json run(const Project & project, const std::filesystem::path & prediction_path, const std::filesystem::path & validation_manifest) const;

private:
    std::span<const TemplateAdapter * const> adapters_;
    std::span<const CollectionAdapter * const> collection_adapters_;
};

} // namespace markov
