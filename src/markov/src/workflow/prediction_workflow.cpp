#include "markov/workflow/prediction_workflow.hpp"

#include "markov/workflow/cpu_sampling_catalog.hpp"

#include "markov/adapters/template_adapter.hpp"
#include "markov/contracts/json_input.hpp"
#include "markov/controller/lightweight_controller.hpp"
#include "markov/execution/simulation.hpp"
#include "markov/performance/communication_cost_model.hpp"
#include "markov/performance/cpu_cost_model.hpp"
#include "markov/performance/device_command_cost_model.hpp"
#include "markov/performance/kernel_cost_model.hpp"
#include "markov/performance/node_costs.hpp"
#include "markov/performance/node_durations.hpp"
#include "markov/performance/transfer_cost_model.hpp"
#include "markov/templates/template_asset.hpp"
#include "markov/templates/templates.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
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
    FrameworkConfiguration configuration;
    std::unique_ptr<TemplateBank> bank;
    std::optional<std::string> duration_error;
};

struct LoadedCostModel {
    std::filesystem::path path;
    std::unique_ptr<CostModel> model;
    std::string framework;
    std::filesystem::path model_path;
    FrameworkConfiguration configuration;
    std::string runtime;
    Environment deployment;
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

nlohmann::json PredictionWorkflow::run(const Project & project, nlohmann::json collection_plan, const std::filesystem::path & catalog_directory) const {
    std::vector<LoadedTemplate> sources;
    NodeDurations durations;
    auto asset_reports = Json::array();
    std::vector<LoadedCostModel> models;
    auto model_reports = Json::array();
    std::optional<std::string> model_error;
    for (const auto & path : project.cost_models) {
        Json report{
            { "path", path.string() }
        };
        try {
            auto metadata = input::read_json(path);
            const auto kind = metadata.at("kind").get<std::string>();
            if (metadata.at("unit") != "ns" || metadata.at("status") != "candidate")
                throw std::invalid_argument("cost model requires an empirical candidate in ns");

            std::unique_ptr<CostModel> model;
            std::string_view query_policy;
            if (kind == "empirical_cpu_service_median") {
                model = std::make_unique<CpuCostModel>(metadata.at("entries"));
                query_policy = "exact named CPU interval and resolved work; directly observed waits/transfers excluded; opaque readiness overlaps device waits";
            }
            else if (kind == "empirical_transfer_median") {
                model = std::make_unique<TransferCostModel>(metadata.at("entries"));
                query_policy = "exact execution boundary, bytes and direction; asynchronous device service or complete synchronous host driver call; preceding "
                               "submission and waits excluded";
            }
            else if (kind == "empirical_device_command_median") {
                model = std::make_unique<DeviceCommandCostModel>(metadata.at("entries"));
                query_policy = "physical device record command with exact adapter task type; CPU submission, waits and collective tasks excluded";
            }
            else if (kind == "empirical_communication_median") {
                model = std::make_unique<CommunicationCostModel>(metadata.at("entries"));
                query_policy = "exact physical task kind, operation, bytes, dtype, transport and rank endpoints; waits and envelopes excluded";
            }
            else if (kind == "empirical_kernel_median") {
                model = std::make_unique<KernelCostModel>(metadata.at("entries"));
                query_policy = "exact tensors and one sampled launch variant; explicit tensor-traversal assumption or matching observed semantic operands";
            }
            else throw std::invalid_argument("unknown cost model kind: " + kind);
            const auto & context = metadata.at("observed_context");
            const auto & deployment = context.at("deployment");
            const auto adapter =
                std::ranges::find_if(adapters_, [&](const TemplateAdapter * item) { return item->name() == context.at("framework").get<std::string_view>(); });
            if (adapter == adapters_.end()) throw std::invalid_argument("cost model framework adapter is not available");
            LoadedCostModel loaded{
                path,
                std::move(model),
                context.at("framework").get<std::string>(),
                context.at("model_path").get<std::string>(),
                (*adapter)->source_configuration(context.at("configuration")),
                deployment.at("runtime").get<std::string>(),
                { deployment.at("service").get<std::string>(), deployment.at("devices").get<std::vector<std::uint32_t>>() }
            };
            // Fitted values are self-contained. Source paths remain provenance;
            // prediction neither rereads samples nor refits the declared model.
            report.update({
                {                "status","loaded"                                          },
                {                  "kind",                                 kind },
                {         "qualification",                    "not_established" },
                { "measurement_readiness", metadata.at("measurement_readiness") },
                {      "measurement_gaps",      metadata.at("measurement_gaps") },
                {           "assumptions",           metadata.at("assumptions") },
                {      "observed_context",                              context },
                {       "source_manifest",       metadata.at("source_manifest") },
                {        "sample_sources",        metadata.at("sample_sources") },
                {          "query_policy",                         query_policy },
                {         "applicability",
                 "same model/framework/deployment and adapter-qualified execution path; target unit work must match; runtime and service transfer not "
                 "established"                                                 }
            });
            models.push_back(std::move(loaded));
        }
        catch (const std::exception & error) {
            model_error = error.what();
            report.update(incomplete("failed", "cost_model", *model_error));
        }
        model_reports.push_back(std::move(report));
    }
    std::int64_t template_load_ns = 0;
    for (const auto & input : project.templates) {
        Json report{
            { "manifest", input.manifest.string() }
        };
        try {
            const auto manifest = markov::input::read_json(input.manifest);
            if (manifest.at("role") != "base") throw std::invalid_argument("prediction templates require role=base");
            const auto adapter =
                std::ranges::find_if(adapters_, [&](const TemplateAdapter * item) { return item->name() == manifest.at("framework").get<std::string_view>(); });
            if (adapter == adapters_.end()) {
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
                                   (*adapter)->source_configuration(source.configuration),
                                   std::make_unique<TemplateBank>(std::move(source)),
                                   std::nullopt };
            report["source"] = loaded.bank->describe(project.diagnostics);

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

    std::vector<TemplateSourceCandidate> banks;
    for (const auto & source : sources) banks.push_back({ source.bank.get(), &source.configuration });

    auto source_coverage = Json::array();
    CpuSamplingCatalog cpu_catalogs(catalog_directory.empty() ? project.output_directory / "catalogs" : catalog_directory);
    auto combinations = Json::array();
    std::size_t combination_index = 0;
    for (const auto & candidate : project.configurations) {
        std::vector<const CostModel *> applicable_models;
        auto model_paths = Json::array();
        auto model_context_gaps = Json::array();
        std::vector<const TemplateBank *> donors;
        std::vector<const TemplateDefinition *> operation_sources;
        const LoadedTemplate * selected = nullptr;
        std::optional<ServingLimits> limits;
        std::optional<Json> configuration_gap;
        const auto adapter = std::ranges::find_if(adapters_, [&](const TemplateAdapter * item) { return item->framework() == candidate.framework; });
        if (adapter == adapters_.end()) configuration_gap = incomplete("unsupported", "implementation", "framework_prediction_not_implemented");
        else {
            const auto & binding = **adapter;
            for (const auto & model : models) {
                std::string_view reason;
                if (model.model_path != project.model_path || model.framework != binding.name()) reason = "cost_model_implementation_mismatch";
                else if (!binding.shares_execution_path(model.configuration, candidate.configuration)) reason = "cost_model_configuration_mismatch";
                else if (model.deployment.devices != candidate.environment.devices || model.deployment.service != candidate.environment.service
                         || model.runtime != "docker")
                    reason = "cost_model_deployment_mismatch";

                // Configuration compatibility permits a query, not a timing
                // guarantee. Actual unit keys still decide cost availability.
                if (!reason.empty()) {
                    model_context_gaps.push_back({
                        {   "path", model.path.string() },
                        { "reason",              reason }
                    });
                    continue;
                }
                applicable_models.push_back(model.model.get());
                model_paths.push_back(model.path.string());
            }
            const auto control = binding.control_limits(candidate.configuration);
            if (!control) configuration_gap = incomplete("unsupported", "implementation", control.error());
            else limits = *control;

            const auto selection = binding.select_sources(candidate, banks);
            donors = selection.donors;
            operation_sources = selection.operation_sources;
            auto coverage = selection.describe();
            coverage["configuration_id"] = candidate.id;
            source_coverage.push_back(std::move(coverage));

            // Cost availability selects among the same qualified initial sources;
            // it never changes structural eligibility or requests more collection.
            for (const auto & source : sources) {
                if (std::find(selection.initial_sources.begin(), selection.initial_sources.end(), source.bank.get()) == selection.initial_sources.end())
                    continue;
                if (!selected || (!durations.contains(selected->input->manifest) && durations.contains(source.input->manifest))) selected = &source;
            }
            if (!configuration_gap && selection.gap) {
                const bool missing = selection.gap->kind == TemplateGapKind::Observation;
                configuration_gap = incomplete(missing ? "needs_data" : "unsupported", missing ? "observation" : "rule", selection.gap->reason);
            }
        }
        if (selected) selected->bank->bind_sources(donors, operation_sources);

        for (const auto & workload : project.request_workloads) {
            Json result;
            try {
                if (configuration_gap) result = *configuration_gap;
                else if (std::any_of(workload.requests.begin(), workload.requests.end(), [](const Request & request) {
                             return std::holds_alternative<std::string>(request.input);
                         }))
                    result = incomplete("unsupported", "implementation", "text_tokenization_not_implemented");
                else if (selected->duration_error) result = incomplete("failed", "node_durations", *selected->duration_error);
                else if (model_error) result = incomplete("failed", "cost_model", *model_error);
                else {
                    const auto cpu_catalog = cpu_catalogs.publish(*selected->bank, donors, operation_sources);
                    // All mutable serving state belongs to this one combination.
                    LightweightController controller(selected->bank->page_size_tokens(), *selected->bank->admission_policy());
                    const NodeCosts costs(durations, applicable_models);
                    const auto graph_path = project.output_directory / ("combination_" + std::to_string(combination_index) + "_graph.json");
                    SimulationRuntime runtime(workload.requests,
                                              controller,
                                              *selected->bank,
                                              costs,
                                              project.diagnostics ? std::optional(graph_path) : std::nullopt);
                    result = runtime.run(*limits);
                    result["performance_requirements"]["cpu_service_catalog"] = cpu_catalog.string();
                }
            }
            catch (const std::exception & error) {
                result = incomplete("failed", "execution", error.what());
            }

            result["configuration_id"] = candidate.id;
            result["request_workload_id"] = workload.id;
            result["scope"] = "modeled";
            // The anchor is distinct from the graph's actual source set.
            // Per-source cost inputs are reported once in assets.
            result["provenance"] = {
                { "source", selected ? Json{ { "manifest", selected->input->manifest.string() } } : Json(nullptr) }
            };
            result["collection_usage_ref"] = "collection_usage";
            result["cost_models"] = model_paths;
            result["cost_model_context_gaps"] = model_context_gaps;
            combinations.push_back(std::move(result));
            ++combination_index;
        }
    }

    collection_plan["source_coverage"] = std::move(source_coverage);
    return {
        {               "task_status",                                                                                                "finished" },
        {              "combinations",                                                                                              combinations },
        {                    "assets",                                                                                             asset_reports },
        {               "cost_models",                                                                                             model_reports },
        {      "collection_performed",                                                                                                     false },
        {          "collection_usage", { { "base", { { "consumed_wall_time_ns", 0 } } }, { "performance", { { "consumed_wall_time_ns", 0 } } } } },
        {                "processing",                                                    { { "template_load_wall_time_ns", template_load_ns } } },
        { "representative_collection",                                                                                           collection_plan }
    };
}

} // namespace markov
