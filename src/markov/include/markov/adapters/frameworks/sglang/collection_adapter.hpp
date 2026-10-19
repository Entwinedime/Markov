#pragma once

#include "markov/adapters/collection_adapter.hpp"

#include <filesystem>
#include <span>
#include <string_view>

namespace markov {

class SGLangCollectionAdapter final : public CollectionAdapter {
public:
    Framework framework() const override { return Framework::SGLang; }
    std::string_view name() const override { return "sglang"; }
    std::string_view service() const override { return "sglang-profile"; }
    FrameworkConfiguration read_configuration(const std::filesystem::path & path, const nlohmann::json & data) const override;
    nlohmann::json configuration(const FrameworkConfiguration & configuration) const override;
    nlohmann::json instrumentation() const override;
    bool supports_target_observation() const override { return true; }
    nlohmann::json sampling_requests(const Candidate & candidate, std::span<const Request> prompts, const nlohmann::json & requirements) const override;
    nlohmann::json workloads(const Candidate & candidate, std::span<const Request> prompts, const nlohmann::json & measurement_requests) const override;
    nlohmann::json collectives(const Candidate & candidate, std::span<const std::filesystem::path> performance_manifests) const override;
};

} // namespace markov
