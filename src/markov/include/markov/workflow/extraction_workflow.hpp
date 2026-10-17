#pragma once

#include <nlohmann/json_fwd.hpp>

#include <filesystem>
#include <span>
#include <string_view>
#include <utility>

namespace markov {

class TemplateAdapter;

class ExtractionWorkflow {
public:
    explicit ExtractionWorkflow(std::span<const TemplateAdapter * const> adapters, std::filesystem::path output_directory = {},
                                std::filesystem::path work_directory = {})
        : adapters_(adapters),
          output_directory_(std::move(output_directory)),
          work_directory_(std::move(work_directory)) {}

    nlohmann::json run(const std::filesystem::path & manifest_path) const;
    nlohmann::json compile(const std::filesystem::path & manifest_path) const;

private:
    const TemplateAdapter * adapter(std::string_view framework) const;

    void publish_manifest(const std::filesystem::path & path, const nlohmann::json & manifest) const;
    std::span<const TemplateAdapter * const> adapters_; // Entry-point owned throughout extraction/compilation.
    std::filesystem::path output_directory_;
    std::filesystem::path work_directory_;
};

} // namespace markov
