#include "markov/adapters/frameworks/sglang/collection_adapter.hpp"

#include "markov/adapters/frameworks/sglang/bindings.hpp"
#include "markov/adapters/frameworks/sglang/sampling_recipes.hpp"
#include "markov/contracts/control.hpp"

#include "markov/contracts/json_input.hpp"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <variant>
#include <vector>

namespace markov {

FrameworkConfiguration SGLangCollectionAdapter::read_configuration(const std::filesystem::path &, const nlohmann::json & data) const {
    input::fields(data, { "parameters", "model_assets" });
    const auto & p = data.at("parameters");
    const auto assets = data.value("model_assets", nlohmann::json::object());
    input::fields(assets, {});
    return SGLangConfiguration::from_parameters(p);
}

nlohmann::json SGLangCollectionAdapter::configuration(const FrameworkConfiguration & configuration) const {
    return {
        {   "parameters", std::get<SGLangConfiguration>(configuration).parameters() },
        { "model_assets",                                  nlohmann::json::object() }
    };
}

nlohmann::json SGLangCollectionAdapter::instrumentation(bool structural) const {
    if (!structural)
        return {
            { "native_regions", SGLangBindings{}.describe_regions() }
        };
    return {
        { "block_bindings",         SGLangBindings{}.describe() },
        { "native_regions", SGLangBindings{}.describe_regions() },
        {         "blocks",        ServingContract{}.describe() }
    };
}

nlohmann::json SGLangCollectionAdapter::capture_executions(const Candidate & candidate, bool reference) const {
    const auto & configuration = std::get<SGLangConfiguration>(candidate.configuration);
    if (reference) {
        if (candidate.environment.devices.size() != configuration.tp_size)
            throw std::invalid_argument("native SGLang reference or communication measurement requires one declared device per TP rank");
        return nlohmann::json::array({ nullptr });
    }

    // Capture the first, repeated and final decoder positions. The native
    // adapter bounds this count by the original model before construction.
    auto executions = nlohmann::json::array();
    // The supported native protocol has an entry role and an ordinary peer.
    // Observe both whole lifecycles instead of cloning entry-role CPU work.
    const auto role_count = configuration.tp_size == 1 ? 1U : 2U;
    for (std::uint32_t rank = 0; rank < role_count; ++rank)
        executions.push_back({
            {               "tp_size", configuration.tp_size },
            {                  "rank",                  rank },
            { "representative_layers",                     3 }
        });
    return executions;
}

nlohmann::json SGLangCollectionAdapter::workloads(const Candidate & candidate, std::span<const Request> prompts,
                                                  const nlohmann::json & measurement_requests) const {
    return SGLangSamplingRecipes{}.workloads(std::get<SGLangConfiguration>(candidate.configuration), prompts, measurement_requests);
}

nlohmann::json SGLangCollectionAdapter::sampling_requests(const Candidate & candidate, std::span<const Request> prompts,
                                                          const nlohmann::json & requirements) const {
    const auto executions = capture_executions(candidate, false);
    std::vector<std::uint32_t> ranks;
    for (const auto & execution : executions) ranks.push_back(execution.at("rank").get<std::uint32_t>());
    return SGLangSamplingRecipes{}.sampling_requests(std::get<SGLangConfiguration>(candidate.configuration), prompts, requirements, ranks);
}

nlohmann::json SGLangCollectionAdapter::collectives(const Candidate & candidate, std::span<const std::filesystem::path> performance_manifests) const {
    return SGLangSamplingRecipes{}.collectives(std::get<SGLangConfiguration>(candidate.configuration), candidate.environment, performance_manifests);
}

} // namespace markov
