#pragma once

#include <nlohmann/json_fwd.hpp>

#include <filesystem>

namespace markov {

class ExtractionWorkflow {
public:
    nlohmann::json run(const std::filesystem::path & manifest_path) const;
    nlohmann::json compile(const std::filesystem::path & manifest_path) const;

private:
    void publish_manifest(const std::filesystem::path & path, const nlohmann::json & manifest) const;
};

} // namespace markov
