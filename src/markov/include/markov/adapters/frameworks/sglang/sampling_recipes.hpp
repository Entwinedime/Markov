#pragma once

#include "markov/contracts/project.hpp"

#include <nlohmann/json_fwd.hpp>

#include <filesystem>
#include <span>

namespace markov {

// Native measurement inputs only; never runs sampling or supplies node costs.
class SGLangSamplingRecipes {
public:
    nlohmann::json workloads(const SGLangConfiguration & configuration, std::span<const Request> prompt_sources,
                             const nlohmann::json & measurement_requests) const;
    nlohmann::json collectives(const SGLangConfiguration & configuration, const Environment & environment,
                               std::span<const std::filesystem::path> performance_manifests) const;
};

} // namespace markov
