#include "markov/workflow/plan.hpp"

#include "markov/adapters/collection_adapter.hpp"
#include "markov/adapters/template_adapter.hpp"
#include "markov/contracts/control.hpp"
#include "markov/workflow/performance_sampling_plan.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace markov {

nlohmann::json Planner::prepare_performance(const Project & project, const nlohmann::json & prediction) const {
    return PerformanceSamplingPlan(collection_adapters_, adapters_).run(project, prediction);
}

namespace {

using Json = nlohmann::json;

Json permission(const CollectionPermission & value) {
    return {
        {         "allow_run",         value.allow_run },
        { "max_wall_time_sec", value.max_wall_time_sec }
    };
}

Json environment(const Environment & value) {
    return {
        { "runtime",      "docker" },
        { "service", value.service },
        { "devices", value.devices }
    };
}

Json requests(std::span<const Request> input, bool native) {
    auto result = Json::array();
    for (const auto & request : input) {
        Json item{
            {            "id",            request.id },
            { "output_tokens", request.output_tokens }
        };
        if (native) item["arrival_time_ns"] = request.arrival_time_ns;
        else item["arrival_offset_us"] = request.arrival_time_ns / 1'000;
        if (const auto * tokens = std::get_if<std::vector<std::uint32_t>>(&request.input)) item["input_tokens"] = *tokens;
        else item["text"] = std::get<std::string>(request.input);
        result.push_back(std::move(item));
    }
    return result;
}

} // namespace

const CollectionAdapter & Planner::collection_adapter(Framework framework) const {
    const auto adapter = std::ranges::find_if(collection_adapters_, [&](const CollectionAdapter * item) { return item->framework() == framework; });
    if (adapter == collection_adapters_.end()) throw std::invalid_argument("framework collection preparation not implemented");
    return **adapter;
}

nlohmann::json Planner::describe(const Project & project) const {
    auto combinations = Json::array();
    for (const auto & candidate : project.configurations) {
        for (const auto & workload : project.request_workloads) {
            auto gaps = Json::array();
            bool supported = true;
            const auto adapter = std::ranges::find_if(adapters_, [&](const TemplateAdapter * item) { return item->framework() == candidate.framework; });
            if (adapter == adapters_.end()) {
                supported = false;
                gaps.push_back({
                    {   "kind",                       "implementation" },
                    { "reason", "framework_prediction_not_implemented" }
                });
            }
            else if (!(*adapter)->control_limits(candidate.configuration)) {
                supported = false;
                gaps.push_back({
                    {   "kind",               "implementation" },
                    { "reason", "control_path_not_implemented" }
                });
            }
            if (project.controller != "lightweight" || project.scope != Scope::Modeled || project.initial_cache != CacheState::Cold) {
                supported = false;
                gaps.push_back({
                    {   "kind",                                  "implementation" },
                    { "reason", "only_lightweight_cold_modeled_scope_implemented" }
                });
            }
            if (std::any_of(workload.requests.begin(), workload.requests.end(), [](const Request & request) {
                    return std::holds_alternative<std::string>(request.input);
                })) {
                supported = false;
                gaps.push_back({
                    {   "kind",                    "implementation" },
                    { "reason", "text_tokenization_not_implemented" }
                });
            }
            gaps.push_back({
                {   "kind",                   "scope" },
                { "reason", "prediction_not_executed" }
            });
            gaps.push_back({
                {   "kind",                            "scope" },
                { "reason", "source_applicability_not_checked" }
            });
            combinations.push_back({
                {    "configuration_id",                             candidate.id },
                { "request_workload_id",                              workload.id },
                {              "status", supported ? "needs_data" : "unsupported" },
                {             "metrics",                                  nullptr },
                {                "gaps",                                     gaps }
            });
        }
    }

    return {
        {               "task_status",                                                                                                          "planned" },
        {                   "project",                                                                                              project.path.string() },
        {                "model_path",                                                                                        project.model_path.string() },
        {              "combinations",                                                                                                       combinations },
        {                     "steps",                                                                                       ServingContract{}.describe() },
        {                "collection", { { "base", permission(project.base_collection) }, { "performance", permission(project.performance_collection) } } },
        { "representative_collection",                                                                                           collection_plan(project) },
        {           "template_assets",                                                                                           project.templates.size() }
    };
}

nlohmann::json Planner::collection_plan(const Project & project) const {
    if (project.controller != "lightweight" || project.scope != Scope::Modeled || project.initial_cache != CacheState::Cold) {
        return {
            { "status",                                     "unsupported" },
            { "reason", "only_lightweight_cold_modeled_scope_implemented" }
        };
    }
    auto groups = Json::array();
    std::map<std::string, Json> exclusions;
    std::vector<std::string> selection_bases;
    for (const auto * adapter : adapters_) {
        std::vector<const Candidate *> candidates;
        for (const auto & candidate : project.configurations)
            if (candidate.framework == adapter->framework()) candidates.push_back(&candidate);

        const auto plan = adapter->configurations(candidates);
        for (const auto & group : plan.at("groups")) groups.push_back(group);
        for (const auto & excluded : plan.at("excluded_configurations")) exclusions.emplace(excluded.at("configuration_id").get<std::string>(), excluded);
        const auto basis = plan.at("selection_basis").get<std::string>();
        if (std::ranges::find(selection_bases, basis) == selection_bases.end()) selection_bases.push_back(basis);
    }

    // Keep exclusions in user declaration order across adapter boundaries.
    auto excluded = Json::array();
    for (const auto & candidate : project.configurations) {
        const auto adapter = std::ranges::find_if(adapters_, [&](const TemplateAdapter * item) { return item->framework() == candidate.framework; });
        if (adapter == adapters_.end()) {
            excluded.push_back({
                { "configuration_id",                                    candidate.id },
                {           "reason", "framework_collection_planning_not_implemented" }
            });
        }
        else if (const auto found = exclusions.find(candidate.id); found != exclusions.end()) excluded.push_back(found->second);
    }

    std::string basis;
    for (const auto & description : selection_bases) {
        if (!basis.empty()) basis += "; ";
        basis += description;
    }
    return {
        {                  "status", groups.empty() ? "unsupported" : "requires_mechanism_coverage" },
        {                  "groups",                                              std::move(groups) },
        { "excluded_configurations",                                            std::move(excluded) },
        {         "selection_basis",                                               std::move(basis) }
    };
}

nlohmann::json Planner::prepare_inputs(const Project & project, const Candidate & candidate) const {
    const auto & adapter = collection_adapter(candidate.framework);
    const auto snapshot = adapter.configuration(candidate.configuration);
    auto values = snapshot.at("parameters");
    values.update(snapshot.at("model_assets"));
    return {
        {             "role",                                                                                                             "base" },
        {        "framework",                                                                                                     adapter.name() },
        { "configuration_id",                                                                                                       candidate.id },
        {       "model_path",                                                                                        project.model_path.string() },
        {   "tokenizer_path",                                                                                    project.tokenizer_path.string() },
        {    "configuration",                                                                                                  std::move(values) },
        {      "environment",                                                                                 environment(candidate.environment) },
        {       "permission",                                                                                permission(project.base_collection) },
        {      "permissions", { { "base", permission(project.base_collection) }, { "performance", permission(project.performance_collection) } } },
        {      "diagnostics",                                                                                                project.diagnostics },
        { "output_directory",                                                                                  project.output_directory.string() }
    };
}

nlohmann::json Planner::prepare_collection(const Project & project, const Candidate & candidate, std::span<const Request> input) const {
    auto result = prepare_inputs(project, candidate);
    result.update(collection_adapter(candidate.framework).instrumentation());
    result["requests"] = requests(input, true);
    return result;
}

nlohmann::json Planner::prepare_representative(const Project & project, const Candidate & candidate) const {
    const auto plan = collection_plan(project);
    for (const auto & group : plan.at("groups")) {
        if (group.at("representative_configuration_id") != candidate.id) continue;
        const auto & recipe = group.at("candidate_recipe");
        if (recipe.at("status") == "not_ready") throw std::invalid_argument(recipe.at("reason").get<std::string>());

        auto prepared = prepare_collection(project, candidate, {});
        prepared["representative_recipe"] = recipe;
        auto groups = Json::array();
        for (const auto & phase : recipe.at("groups")) {
            auto members = Json::array();
            const auto & offsets = phase.at("prompt_page_offsets");
            for (std::size_t member = 0; member < offsets.size(); ++member) {
                const auto index = prepared.at("requests").size();
                prepared["requests"].push_back({
                    {                 "id", phase.at("purpose").get<std::string>() + "-" + std::to_string(member) },
                    { "prompt_page_offset",                                                    offsets.at(member) },
                    {      "output_tokens",                                  phase.at("output_tokens").at(member) }
                });
                members.push_back(index);
            }
            groups.push_back(std::move(members));
        }
        prepared["submission_groups"] = std::move(groups);
        return prepared;
    }
    throw std::invalid_argument("configuration is not a planned representative");
}

nlohmann::json Planner::prepare_target(const Project & project, const Candidate & candidate, const RequestWorkload & workload, std::int64_t budget_sec,
                                       std::string_view purpose) const {
    if (purpose != "ports" && purpose != "structure") throw std::invalid_argument("target observation purpose must be ports or structure");
    if (!collection_adapter(candidate.framework).supports_target_observation())
        throw std::invalid_argument("target observation not implemented for this framework");
    if (budget_sec <= 0 || budget_sec > std::numeric_limits<std::int64_t>::max() / 1'000'000'000)
        throw std::invalid_argument("target observation requires a positive nanosecond-representable budget");

    auto result = prepare_collection(project, candidate, workload.requests);
    result["role"] = "validation";
    result["observation_purpose"] = std::string(purpose);
    result["candidate_id"] = candidate.id;
    result["request_workload_id"] = workload.id;
    result["permission"] = permission({ true, budget_sec });
    result["permissions"] = {
        { "validation", result["permission"] }
    };
    result["output_directory"] = project.output_directory.string();
    return result;
}

nlohmann::json Planner::prepare_prediction(const Project & project) const {
    auto configuration_files = Json::object();
    auto configurations = Json::array();
    for (std::size_t index = 0; index < project.configurations.size(); ++index) {
        const auto & candidate = project.configurations[index];
        const auto filename = "configuration-" + std::to_string(index) + ".json";
        const auto & adapter = collection_adapter(candidate.framework);
        configuration_files[filename] = adapter.configuration(candidate.configuration);
        configurations.push_back({
            {          "id",                       candidate.id },
            {   "framework",                     adapter.name() },
            { "environment", environment(candidate.environment) },
            {      "config",                           filename }
        });
    }

    auto workload_files = Json::object();
    auto workloads = Json::array();
    for (std::size_t index = 0; index < project.request_workloads.size(); ++index) {
        const auto & workload = project.request_workloads[index];
        const auto filename = "workload-" + std::to_string(index) + ".json";
        workload_files[filename] = {
            { "requests", requests(workload.requests, false) }
        };
        workloads.push_back({
            {   "id", workload.id },
            { "path",    filename }
        });
    }

    auto templates = Json::array();
    for (const auto & source : project.templates) {
        templates.push_back({
            {       "manifest",                                                      source.manifest.string() },
            { "node_durations", source.node_durations ? Json(source.node_durations->string()) : Json(nullptr) }
        });
    }
    auto performances = Json::array();
    for (const auto & path : project.performance_manifests) performances.push_back(path.string());
    auto models = Json::array();
    for (const auto & path : project.cost_models) models.push_back(path.string());
    Json input{
        {             "model",                 { { "path", project.model_path.string() }, { "tokenizer_path", project.tokenizer_path.string() } } },
        {    "configurations",                                                                                                     configurations },
        { "request_workloads",                                                                                                          workloads },
        {        "simulation",    { { "controller", project.controller }, { "scope", project.scope == Scope::Modeled ? "modeled" : "http_e2e" } } },
        {     "initial_state",                                       { { "cache", project.initial_cache == CacheState::Cold ? "cold" : "warm" } } },
        {        "collection", { { "base", permission(project.base_collection) }, { "performance", permission(project.performance_collection) } } },
        {            "assets",               { { "templates", templates }, { "performance_manifests", performances }, { "cost_models", models } } },
        {            "output",                     { { "directory", project.output_directory.string() }, { "diagnostics", project.diagnostics } } }
    };
    return {
        {           "project",               input },
        {    "configurations", configuration_files },
        { "request_workloads",      workload_files }
    };
}

nlohmann::json Planner::prepare_sampling(const Project & project, const Candidate & candidate, const RequestWorkload & prompts,
                                         const Json & measurement_requests) const {
    if (project.controller != "lightweight") throw std::invalid_argument("sampling requires lightweight control");
    auto grid = collection_adapter(candidate.framework).workloads(candidate, prompts.requests, measurement_requests);
    auto prepared = prepare_inputs(project, candidate);
    prepared["role"] = "performance";
    prepared["permission"] = permission(project.performance_collection);
    prepared["measurement_grid"] = std::move(grid);
    prepared["measurement_requests"] = measurement_requests;
    return prepared;
}

nlohmann::json Planner::prepare_communication(const Project & project, const Candidate & candidate) const {
    auto grid = collection_adapter(candidate.framework).collectives(candidate, project.performance_manifests);
    if (grid.empty() && project.performance_collection.allow_run)
        throw std::invalid_argument("communication sampling needs independent device measurements with collective work domains");

    auto prepared = prepare_inputs(project, candidate);
    prepared["role"] = "performance";
    prepared["permission"] = permission(project.performance_collection);
    prepared["measurement_grid"] = std::move(grid);
    return prepared;
}

} // namespace markov
