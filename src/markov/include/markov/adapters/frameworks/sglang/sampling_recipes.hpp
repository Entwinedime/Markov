#pragma once

#include "markov/adapters/frameworks/sglang/configuration.hpp"
#include "markov/contracts/environment.hpp"
#include "markov/contracts/request.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <filesystem>
#include <span>

namespace markov {

// Native measurement inputs only; never runs sampling or supplies node costs.
class SGLangSamplingRecipes {
public:
    nlohmann::json sampling_requests(const SGLangConfiguration & configuration, std::span<const Request> prompt_sources, const nlohmann::json & requirements,
                                     std::span<const std::uint32_t> observed_ranks) const;
    nlohmann::json workloads(const SGLangConfiguration & configuration, std::span<const Request> prompt_sources,
                             const nlohmann::json & measurement_requests) const;
    nlohmann::json collectives(const SGLangConfiguration & configuration, const Environment & environment,
                               std::span<const std::filesystem::path> performance_manifests) const;

private:
    static nlohmann::json make_request(const Request & source, std::uint32_t prompt_tokens, std::uint32_t output_tokens, std::uint32_t release_after_tokens);
};

} // namespace markov
