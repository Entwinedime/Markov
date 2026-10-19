#pragma once

#include <nlohmann/json_fwd.hpp>

#include <filesystem>
#include <span>
#include <vector>

namespace markov {

class TemplateBank;
struct TemplateDefinition;

// Named source intervals guide CPU measurement. Target work and cost coverage
// belong to execution; source batch features are not sampling requirements.
class CpuSamplingCatalog {
public:
    explicit CpuSamplingCatalog(std::filesystem::path output_directory);
    std::filesystem::path publish(const TemplateBank & primary, std::span<const TemplateBank * const> donors,
                                  std::span<const TemplateDefinition * const> operation_sources);

private:
    struct Catalog {
        std::vector<const TemplateDefinition *> definitions;
        std::filesystem::path path;
    };

    static nlohmann::json collect(std::span<const TemplateDefinition * const> definitions);
    std::filesystem::path output_directory_;
    std::vector<Catalog> catalogs_; // Definitions remain owned by this prediction workflow.
};

} // namespace markov
