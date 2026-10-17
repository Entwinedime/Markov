#pragma once

#include "markov/adapters/collection_adapter.hpp"

namespace markov {

class KTransformersCollectionAdapter final : public CollectionAdapter {
public:
    Framework framework() const override { return Framework::KTransformers; }
    std::string_view name() const override { return "ktransformers"; }
    std::string_view service() const override { return "ktransformers-profile"; }
    FrameworkConfiguration read_configuration(const std::filesystem::path & path, const nlohmann::json & data) const override;
    nlohmann::json configuration(const FrameworkConfiguration & configuration) const override;
    nlohmann::json instrumentation() const override;
    bool supports_target_observation() const override { return false; }
    nlohmann::json sampling_requests(const Candidate & candidate, std::span<const Request> prompts, const nlohmann::json & requirements) const override;
    nlohmann::json workloads(const Candidate & candidate, std::span<const Request> prompts, const nlohmann::json & measurement_requests) const override;
    nlohmann::json collectives(const Candidate & candidate, std::span<const std::filesystem::path> performance_manifests) const override;
};

} // namespace markov
