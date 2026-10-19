#include "markov/adapters/template_adapter.hpp"

#include <nlohmann/json.hpp>

#include <set>
#include <string>
#include <utility>

namespace markov {

nlohmann::json TemplateSourceSelection::describe() const {
    auto donor_manifests = nlohmann::json::array();
    for (const auto * source : donors) donor_manifests.push_back(source->source().manifest_path.string());
    auto initialization_manifests = nlohmann::json::array();
    for (const auto * source : initial_sources) initialization_manifests.push_back(source->source().manifest_path.string());

    std::set<std::string> operation_manifests;
    for (const auto * definition : operation_sources) operation_manifests.insert(definition->source_manifest.string());

    // Report the selection itself. Actual instantiation owns target support;
    // a second inventory of source mechanisms cannot qualify that execution.
    nlohmann::json result{
        {                        "scope", "source_identity_policy_capacities_and_initialization" },
        {                       "status",                                            "available" },
        {              "donor_manifests",                             std::move(donor_manifests) },
        {     "initialization_manifests",                    std::move(initialization_manifests) },
        { "operation_fragment_manifests",                         std::move(operation_manifests) }
    };
    if (gap) {
        result["status"] = gap->kind == TemplateGapKind::Observation ? "needs_data" : "unsupported";
        result["reason"] = gap->reason;
    }
    return result;
}

} // namespace markov
