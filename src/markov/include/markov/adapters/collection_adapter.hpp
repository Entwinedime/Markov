#pragma once

#include "markov/adapters/configuration.hpp"
#include "markov/contracts/request.hpp"

#include <nlohmann/json_fwd.hpp>

#include <filesystem>
#include <span>
#include <string_view>

namespace markov {

// Native input preparation is independent of prediction support. In particular,
// a framework may supply real captures without a qualified template consumer.
class CollectionAdapter {
public:
    virtual ~CollectionAdapter() = default;

    virtual Framework framework() const = 0;
    virtual std::string_view name() const = 0;
    virtual std::string_view service() const = 0;
    virtual FrameworkConfiguration read_configuration(const std::filesystem::path & path, const nlohmann::json & data) const = 0;
    virtual nlohmann::json configuration(const FrameworkConfiguration & configuration) const = 0;
    virtual nlohmann::json instrumentation() const = 0;
    virtual bool supports_target_observation() const = 0;
    // Pure request planning. Proposed work points still require native feature coverage.
    virtual nlohmann::json sampling_requests(const Candidate & candidate, std::span<const Request> prompts, const nlohmann::json & requirements) const = 0;
    virtual nlohmann::json workloads(const Candidate & candidate, std::span<const Request> prompts, const nlohmann::json & measurement_requests) const = 0;
    virtual nlohmann::json collectives(const Candidate & candidate, std::span<const std::filesystem::path> performance_manifests) const = 0;
};

} // namespace markov
