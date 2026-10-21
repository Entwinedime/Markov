#include "markov/workflow/structure_comparison_workflow.hpp"

#include "markov/adapters/collection_adapter.hpp"
#include "markov/adapters/template_adapter.hpp"
#include "markov/contracts/json_input.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace markov {

nlohmann::json StructureComparisonWorkflow::run(const Project & project, const std::filesystem::path & prediction_path,
                                                const std::filesystem::path & validation_manifest) const {
    const auto manifest = input::read_json(validation_manifest);
    if (manifest.at("role") != "validation" || manifest.at("observation_purpose") != "structure")
        throw std::invalid_argument("structure comparison requires an independent structure validation manifest");

    const auto prepared = input::read_json(input::resolve_path(validation_manifest, manifest.at("input")));
    const auto configuration_id = prepared.at("configuration_id").get<std::string>();
    const auto workload_id = prepared.at("request_workload_id").get<std::string>();
    const auto prediction = input::read_json(prediction_path);
    const auto combination = std::ranges::find_if(prediction.at("combinations"), [&](const auto & item) {
        return item.at("configuration_id") == configuration_id && item.at("request_workload_id") == workload_id;
    });
    if (combination == prediction.at("combinations").end()) throw std::invalid_argument("validation combination is absent from the fixed prediction");

    nlohmann::json result{
        {    "configuration_id",                                        configuration_id },
        { "request_workload_id",                                             workload_id },
        {             "purpose",                                             "structure" },
        {              "status",                                        "not_comparable" },
        { "validation_manifest", std::filesystem::absolute(validation_manifest).string() }
    };
    const auto & candidate = project.configuration(configuration_id);
    const auto & workload = project.workload(workload_id);
    const auto collection = std::ranges::find_if(collection_adapters_, [&](const auto * item) { return item->framework() == candidate.framework; });
    const auto adapter = std::ranges::find_if(adapters_, [&](const auto * item) { return item->framework() == candidate.framework; });
    if (adapter == adapters_.end() || collection == collection_adapters_.end()) {
        result["reason"] = "framework_structure_observation_not_implemented";
        return result;
    }

    const auto snapshot = (*collection)->configuration(candidate.configuration);
    auto parameters = snapshot.at("parameters");
    parameters.update(snapshot.at("model_assets"));
    auto requests = nlohmann::json::array();
    for (const auto & request : workload.requests) {
        nlohmann::json member{
            {              "id",              request.id },
            { "arrival_time_ns", request.arrival_time_ns },
            {   "output_tokens",   request.output_tokens }
        };
        if (const auto * tokens = std::get_if<std::vector<std::uint32_t>>(&request.input)) member["input_tokens"] = *tokens;
        else member["text"] = std::get<std::string>(request.input);
        requests.push_back(std::move(member));
    }
    // This is the one snapshot-matching boundary for structure observations.
    if (prepared.at("role") != "validation" || prepared.at("observation_purpose") != "structure" || prepared.at("framework") != (*adapter)->name()
        || manifest.at("framework") != prepared.at("framework") || prepared.at("model_configuration") != project.model_configuration
        || prepared.at("configuration") != parameters || Environment::read(prepared.at("environment")).describe() != candidate.environment.describe()
        || prepared.at("requests") != requests) {
        result["reason"] = "validation_input_mismatch";
        return result;
    }
    if (combination->at("status") != "complete") {
        result["reason"] = "prediction_incomplete";
        return result;
    }
    const auto & graph = combination->at("structure").at("graph");
    if (!graph.contains("artifact")) {
        result["reason"] = "fixed_prediction_has_no_diagnostic_graph";
        return result;
    }
    const auto target_graph = input::resolve_path(prediction_path, graph.at("artifact"));
    if (!std::filesystem::is_regular_file(target_graph)) {
        result["reason"] = "fixed_prediction_graph_missing";
        return result;
    }

    result.update((*adapter)->compare_structure(target_graph,
                                                input::read_json(input::resolve_path(validation_manifest, manifest.at("observations"))),
                                                input::read_json(input::resolve_path(validation_manifest, manifest.at("environment")))));
    auto examples = nlohmann::json::array();
    for (std::size_t index = 0; index < std::min<std::size_t>(5, manifest.at("gaps").size()); ++index) examples.push_back(manifest.at("gaps")[index]);
    result["normalization_gaps"] = {
        {    "total", manifest.at("gaps").size() },
        {    "shown",            examples.size() },
        { "examples",        std::move(examples) }
    };
    if (!manifest.at("gaps").empty() && result.at("status") == "compared") result["status"] = "partial";
    result["validation_used_for_templates_or_fitting"] = false;
    return result;
}

} // namespace markov
