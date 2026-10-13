#include "markov/adapters/frameworks/sglang/collection_adapter.hpp"

#include "markov/adapters/frameworks/sglang/bindings.hpp"
#include "markov/adapters/frameworks/sglang/sampling_recipes.hpp"
#include "markov/contracts/control.hpp"

#include "markov/contracts/json_input.hpp"

#include <nlohmann/json.hpp>

#include <variant>

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

nlohmann::json SGLangCollectionAdapter::instrumentation() const {
    return {
        { "block_bindings",         SGLangBindings{}.describe() },
        { "native_regions", SGLangBindings{}.describe_regions() },
        {         "blocks",        ServingContract{}.describe() }
    };
}

nlohmann::json SGLangCollectionAdapter::workloads(const Candidate & candidate, std::span<const Request> prompts,
                                                  const nlohmann::json & measurement_requests) const {
    return SGLangSamplingRecipes{}.workloads(std::get<SGLangConfiguration>(candidate.configuration), prompts, measurement_requests);
}

nlohmann::json SGLangCollectionAdapter::collectives(const Candidate & candidate, std::span<const std::filesystem::path> performance_manifests) const {
    return SGLangSamplingRecipes{}.collectives(std::get<SGLangConfiguration>(candidate.configuration), candidate.environment, performance_manifests);
}

} // namespace markov
