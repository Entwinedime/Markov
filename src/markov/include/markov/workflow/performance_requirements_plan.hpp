#pragma once

#include <nlohmann/json_fwd.hpp>

#include <filesystem>
#include <span>
#include <vector>

namespace markov {

class TemplateBank;
struct TemplateDefinition;
struct Request;
struct ServingLimits;

// Potential named work and declared relations before timing-dependent batches
// are known. Actual target points and final CPU cuts belong to execution.
class PerformanceRequirementsPlan {
public:
    explicit PerformanceRequirementsPlan(std::filesystem::path output_directory);

    nlohmann::json run(const TemplateBank & primary, std::span<const TemplateBank * const> donors,
                       std::span<const TemplateDefinition * const> operation_sources, std::span<const Request> requests, const ServingLimits & limits);

private:
    struct Catalog {
        std::vector<const TemplateDefinition *> definitions;
        std::filesystem::path path;
    };

    std::filesystem::path output_directory_;
    std::vector<Catalog> catalogs_; // Borrowed definition identities live through this workflow run.
};

} // namespace markov
