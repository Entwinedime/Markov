#include "markov/workflow/prediction_workflow.hpp"

#include "markov/adapters/frameworks/sglang/bindings.hpp"
#include "markov/controller/lightweight_controller.hpp"
#include "markov/execution/simulation.hpp"
#include "markov/performance/node_durations.hpp"
#include "markov/templates/template_asset.hpp"
#include "markov/templates/templates.hpp"
#include "markov/workflow/plan.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace markov {
namespace {

using Json = nlohmann::json;

struct LoadedTemplate {
    const TemplateInput * input; // Borrowed from this run's immutable project.
    SGLangConfiguration configuration;
    std::unique_ptr<TemplateBank> bank;
    std::optional<std::string> duration_error;
};

Json incomplete(std::string_view status, std::string_view kind, std::string_view reason) {
    return {
        {      "status",                                                      status },
        {     "metrics",                                                     nullptr },
        { "assumptions",                                               Json::array() },
        {        "gaps", Json::array({ { { "kind", kind }, { "reason", reason } } }) }
    };
}

} // namespace

nlohmann::json PredictionWorkflow::run(const Project & project) const {
    std::vector<LoadedTemplate> sources;
    NodeDurations durations;
    auto asset_reports = Json::array();
    std::int64_t template_load_ns = 0;
    for (const auto & input : project.templates) {
        Json report{
            { "manifest", input.manifest.string() }
        };
        try {
            std::ifstream stream(input.manifest);
            stream.exceptions(std::ios::failbit | std::ios::badbit);
            const auto manifest = Json::parse(stream);
            if (manifest.at("role") != "base") throw std::invalid_argument("prediction templates require role=base");
            if (manifest.at("framework") != "sglang") {
                report.update(incomplete("unsupported", "implementation", "framework_prediction_not_implemented"));
                asset_reports.push_back(std::move(report));
                continue;
            }

            const auto asset = manifest.find("compiled_templates");
            if (asset == manifest.end() || asset->is_null() || !std::filesystem::is_regular_file(input.manifest.parent_path() / asset->get<std::string>())) {
                report.update(incomplete("needs_data", "observation", "compiled_templates_missing"));
                asset_reports.push_back(std::move(report));
                continue;
            }

            const auto started = std::chrono::steady_clock::now();
            auto source = TemplateAsset::read(input.manifest.parent_path() / asset->get<std::string>());
            template_load_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count();
            if (source.manifest_path != input.manifest || source.model_path != project.model_path)
                throw std::invalid_argument("compiled source identity or model differs from the declared asset");

            LoadedTemplate loaded{ &input,
                                   SGLangConfiguration::from_parameters(source.configuration),
                                   std::make_unique<TemplateBank>(std::move(source)),
                                   std::nullopt };
            report["source"] = loaded.bank->describe();

            // A bad cost input must not turn reusable structure into a request
            // for native collection. Preserve the source and isolate the error.
            if (input.node_durations) {
                try {
                    durations.load(*input.node_durations, input.manifest);
                    report["node_durations"] = durations.describe(input.manifest);
                }
                catch (const std::exception & error) {
                    loaded.duration_error = error.what();
                    report["node_durations"] = incomplete("failed", "node_durations", *loaded.duration_error);
                }
            }

            sources.push_back(std::move(loaded));
            report["status"] = "loaded";
        }
        catch (const std::exception & error) {
            report.update(incomplete("failed", "asset", error.what()));
        }
        asset_reports.push_back(std::move(report));
    }

    auto combinations = Json::array();
    std::size_t combination_index = 0;
    for (const auto & candidate : project.configurations) {
        std::vector<const TemplateBank *> donors;
        const LoadedTemplate * selected = nullptr;
        std::optional<ServingLimits> limits;
        std::optional<Json> configuration_gap;
        if (candidate.framework != Framework::SGLang) configuration_gap = incomplete("unsupported", "implementation", "framework_prediction_not_implemented");
        else {
            const auto & configuration = std::get<SGLangConfiguration>(candidate.configuration);
            const auto control = SGLangBindings{}.control_limits(configuration);
            if (!control) configuration_gap = incomplete("unsupported", "implementation", control.error());
            else limits = *control;

            // Declared asset order breaks ties; a declared duration source takes
            // precedence over an otherwise compatible source lacking durations.
            bool deployment_available = false;
            std::optional<TemplateGap> source_gap;
            for (const auto & source : sources) {
                const auto & deployment = source.bank->deployment();
                if (deployment.service != candidate.environment.service || deployment.devices != candidate.environment.devices) continue;
                deployment_available = true;
                if (!SGLangBindings{}.supports_configuration_change(source.configuration, configuration)) continue;
                const auto qualification = source.bank->qualify(configuration.max_running_requests);
                if (!qualification) {
                    if (!source_gap) source_gap = qualification.error();
                    continue;
                }
                donors.push_back(source.bank.get());
                const auto initialization = source.bank->qualify_initialization(configuration.max_running_requests);
                if (!initialization) {
                    if (!source_gap) source_gap = initialization.error();
                    continue;
                }
                if (!selected || (!durations.contains(selected->input->manifest) && durations.contains(source.input->manifest))) selected = &source;
            }
            if (!configuration_gap && !selected) {
                if (source_gap) {
                    const bool missing = source_gap->kind == TemplateGapKind::Observation;
                    configuration_gap = incomplete(missing ? "needs_data" : "unsupported", missing ? "observation" : "rule", source_gap->reason);
                }
                else
                    configuration_gap = deployment_available ? incomplete("unsupported", "implementation", "parameter_change_outside_admission_rule")
                                                             : incomplete("needs_data", "observation", "compatible_template_missing");
            }
        }
        if (project.controller != "lightweight" || project.scope != Scope::Modeled || project.initial_cache != CacheState::Cold)
            configuration_gap = incomplete("unsupported", "scope", "only_lightweight_cold_modeled_scope_implemented");

        if (selected) selected->bank->bind_sources(donors);

        for (const auto & workload : project.request_workloads) {
            Json result;
            try {
                if (configuration_gap) result = *configuration_gap;
                else if (std::any_of(workload.requests.begin(), workload.requests.end(), [](const Request & request) {
                             return std::holds_alternative<std::string>(request.input);
                         }))
                    result = incomplete("unsupported", "implementation", "text_tokenization_not_implemented");
                else if (selected->duration_error) result = incomplete("failed", "node_durations", *selected->duration_error);
                else if (!durations.contains(selected->input->manifest)) result = incomplete("needs_data", "input", "node_durations_missing");
                else {
                    // All mutable serving state belongs to this one combination.
                    LightweightController controller(selected->bank->page_size_tokens(), *selected->bank->admission_policy());
                    const auto graph_path = project.output_directory / ("combination_" + std::to_string(combination_index) + "_graph.json");
                    SimulationRuntime runtime(workload.requests,
                                              controller,
                                              *selected->bank,
                                              durations,
                                              project.diagnostics ? std::optional(graph_path) : std::nullopt);
                    result = runtime.run(*limits);
                }
            }
            catch (const std::exception & error) {
                result = incomplete("failed", "execution", error.what());
            }

            result["configuration_id"] = candidate.id;
            result["request_workload_id"] = workload.id;
            result["scope"] = project.scope == Scope::Modeled ? "modeled" : "http_e2e";
            // The anchor is distinct from the graph's actual source set.
            // Per-source cost inputs are reported once in assets.
            result["provenance"] = {
                { "source", selected ? Json{ { "manifest", selected->input->manifest.string() } } : Json(nullptr) }
            };
            result["collection_usage_ref"] = "collection_usage";
            combinations.push_back(std::move(result));
            ++combination_index;
        }
    }

    return {
        {               "task_status",                                                                                                "finished" },
        {              "combinations",                                                                                              combinations },
        {                    "assets",                                                                                             asset_reports },
        {      "collection_performed",                                                                                                     false },
        {          "collection_usage", { { "base", { { "consumed_wall_time_ns", 0 } } }, { "performance", { { "consumed_wall_time_ns", 0 } } } } },
        {                "processing",                                                    { { "template_load_wall_time_ns", template_load_ns } } },
        { "representative_collection",                                                                        Planner{}.collection_plan(project) }
    };
}

} // namespace markov
