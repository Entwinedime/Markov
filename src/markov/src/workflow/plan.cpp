#include "markov/workflow/plan.hpp"

#include "markov/adapters/frameworks/sglang/bindings.hpp"
#include "markov/adapters/frameworks/sglang/collection_planner.hpp"
#include "markov/adapters/frameworks/sglang/sampling_recipes.hpp"
#include "markov/contracts/control.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace markov {
namespace {

using Json = nlohmann::json;

// Serialization is the external report boundary; algorithms use typed configuration.
Json parameters(const FrameworkConfiguration & configuration) {
    if (const auto * p = std::get_if<SGLangConfiguration>(&configuration)) { return p->parameters(); }

    const auto & p = std::get<KTransformersConfiguration>(configuration);
    return {
        {         "backend_type",                  p.backend_type },
        {                   "tp",                            p.tp },
        {       "max_batch_size",                p.max_batch_size },
        {           "chunk_size",                    p.chunk_size },
        {            "cpu_infer",                     p.cpu_infer },
        {           "cache_lens",                    p.cache_lens },
        {       "sched_strategy",                p.sched_strategy },
        {       "use_cuda_graph",                p.use_cuda_graph },
        {            "gguf_path",            p.gguf_path.string() },
        { "optimize_config_path", p.optimize_config_path.string() }
    };
}

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

nlohmann::json Planner::describe(const Project & project) const {
    auto combinations = Json::array();
    for (const auto & candidate : project.configurations) {
        for (const auto & workload : project.request_workloads) {
            auto gaps = Json::array();
            bool supported = true;
            if (candidate.framework != Framework::SGLang) {
                supported = false;
                gaps.push_back({
                    {   "kind",                       "implementation" },
                    { "reason", "framework_prediction_not_implemented" }
                });
            }
            else if (!SGLangBindings{}.control_limits(std::get<SGLangConfiguration>(candidate.configuration))) {
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
    return SGLangCollectionPlanner{}.configurations(project.configurations);
}

nlohmann::json Planner::prepare_collection(const Project & project, const Candidate & candidate, std::span<const Request> input) const {
    const bool sglang = candidate.framework == Framework::SGLang;
    return {
        { "block_bindings", sglang ? SGLangBindings{}.describe() : Json::array() },
        { "native_regions", sglang ? SGLangBindings{}.describe_regions() : Json(nullptr) },
        { "blocks", sglang ? ServingContract{}.describe() : Json::array() },
        { "role", "base" },
        { "framework", sglang ? "sglang" : "ktransformers" },
        { "configuration_id", candidate.id },
        { "model_path", project.model_path.string() },
        { "tokenizer_path", project.tokenizer_path.string() },
        { "configuration", parameters(candidate.configuration) },
        { "requests", requests(input, true) },
        { "environment", environment(candidate.environment) },
        { "permission", permission(project.base_collection) },
        { "permissions", { { "base", permission(project.base_collection) }, { "performance", permission(project.performance_collection) } } },
        { "diagnostics", project.diagnostics },
        { "output_directory", project.output_directory.string() }
    };
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

nlohmann::json Planner::prepare_target(const Project & project, const Candidate & candidate, const RequestWorkload & workload, std::int64_t budget_sec) const {
    if (candidate.framework != Framework::SGLang) throw std::invalid_argument("target observation currently requires SGLang");
    if (budget_sec <= 0 || budget_sec > std::numeric_limits<std::int64_t>::max() / 1'000'000'000)
        throw std::invalid_argument("target observation requires a positive nanosecond-representable budget");

    auto result = prepare_collection(project, candidate, workload.requests);
    result["role"] = "validation";
    result["candidate_id"] = candidate.id;
    result["request_workload_id"] = workload.id;
    result["permission"] = permission({ true, budget_sec });
    result["permissions"] = {
        { "validation", result["permission"] }
    };
    result["output_directory"] = (project.output_directory / "validation").string();
    return result;
}

nlohmann::json Planner::prepare_prediction(const Project & project) const {
    auto configuration_files = Json::object();
    auto configurations = Json::array();
    for (std::size_t index = 0; index < project.configurations.size(); ++index) {
        const auto & candidate = project.configurations[index];
        const auto filename = "configuration-" + std::to_string(index) + ".json";
        auto values = parameters(candidate.configuration);
        auto assets = Json::object();
        if (const auto * native = std::get_if<KTransformersConfiguration>(&candidate.configuration)) {
            assets = {
                {            "gguf_path",            native->gguf_path.string() },
                { "optimize_config_path", native->optimize_config_path.string() }
            };
            values.erase("gguf_path");
            values.erase("optimize_config_path");
        }
        configuration_files[filename] = {
            {   "parameters", values },
            { "model_assets", assets }
        };
        configurations.push_back({
            {          "id",                                                          candidate.id },
            {   "framework", candidate.framework == Framework::SGLang ? "sglang" : "ktransformers" },
            { "environment",                                    environment(candidate.environment) },
            {      "config",                                                              filename }
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
    Json input{
        {             "model",                 { { "path", project.model_path.string() }, { "tokenizer_path", project.tokenizer_path.string() } } },
        {    "configurations",                                                                                                     configurations },
        { "request_workloads",                                                                                                          workloads },
        {        "simulation",    { { "controller", project.controller }, { "scope", project.scope == Scope::Modeled ? "modeled" : "http_e2e" } } },
        {     "initial_state",                                       { { "cache", project.initial_cache == CacheState::Cold ? "cold" : "warm" } } },
        {        "collection", { { "base", permission(project.base_collection) }, { "performance", permission(project.performance_collection) } } },
        {            "assets",                                          { { "templates", templates }, { "performance_manifests", performances } } },
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
    if (candidate.framework != Framework::SGLang) throw std::invalid_argument("independent sampling currently implements SGLang");
    if (project.controller != "lightweight") throw std::invalid_argument("sampling requires lightweight control");
    auto grid = SGLangSamplingRecipes{}.workloads(std::get<SGLangConfiguration>(candidate.configuration), prompts.requests, measurement_requests);
    auto prepared = prepare_collection(project, candidate, {});
    prepared["role"] = "performance";
    prepared["permission"] = permission(project.performance_collection);
    prepared.erase("requests");
    prepared["measurement_grid"] = std::move(grid);
    prepared["measurement_requests"] = measurement_requests;
    return prepared;
}

nlohmann::json Planner::prepare_communication(const Project & project, const Candidate & candidate) const {
    if (candidate.framework != Framework::SGLang) throw std::invalid_argument("communication sampling currently implements SGLang");
    auto grid =
        SGLangSamplingRecipes{}.collectives(std::get<SGLangConfiguration>(candidate.configuration), candidate.environment, project.performance_manifests);
    if (grid.empty() && project.performance_collection.allow_run)
        throw std::invalid_argument("communication sampling needs independent device measurements with collective work domains");

    auto prepared = prepare_collection(project, candidate, {});
    prepared["role"] = "performance";
    prepared["permission"] = permission(project.performance_collection);
    prepared.erase("requests");
    prepared["measurement_grid"] = std::move(grid);
    return prepared;
}

} // namespace markov
