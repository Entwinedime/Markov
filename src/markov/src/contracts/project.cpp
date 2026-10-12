#include "markov/contracts/project.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace markov {
namespace {

using Json = nlohmann::json;
using Path = std::filesystem::path;

Json read_json(const Path & path) {
    std::ifstream stream;
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    stream.open(path);

    try {
        return Json::parse(stream);
    }
    catch (const Json::exception & error) {
        throw std::invalid_argument(path.string() + ": " + error.what());
    }
}

void fields(const Json & value, std::initializer_list<std::string_view> names) {
    if (!value.is_object()) { throw std::invalid_argument("expected a JSON object"); }

    for (const auto & item : value.items()) {
        if (std::find(names.begin(), names.end(), item.key()) == names.end()) { throw std::invalid_argument("unknown field: " + item.key()); }
    }
}

std::string string_value(const Json & value) {
    const auto result = value.get<std::string>();
    if (result.empty()) { throw std::invalid_argument("expected a nonempty string"); }

    return result;
}

// JSON numeric conversion otherwise silently truncates floats or narrows integers.
std::int64_t integer(const Json & value, std::int64_t minimum, std::int64_t maximum) {
    if (!value.is_number_integer() || (value.is_number_unsigned() && value.get<std::uint64_t>() > static_cast<std::uint64_t>(maximum))) {
        throw std::invalid_argument("expected an integer in the declared range");
    }

    const auto result = value.get<std::int64_t>();
    if (result < minimum || result > maximum) { throw std::invalid_argument("integer outside the declared range"); }

    return result;
}

std::uint32_t positive(const Json & value) { return static_cast<std::uint32_t>(integer(value, 1, std::numeric_limits<std::uint32_t>::max())); }

Path resolve_path(const Path & declaring_file, const Json & value) { return (declaring_file.parent_path() / string_value(value)).lexically_normal(); }

std::vector<Request> read_workload(const Path & path) {
    const auto data = read_json(path);
    fields(data, { "requests" });
    const auto & requests = data.at("requests");
    if (!requests.is_array() || requests.empty()) { throw std::invalid_argument("workload.requests must be a nonempty array"); }

    std::set<std::string> ids;
    std::vector<Request> result;
    for (const auto & item : requests) {
        fields(item, { "id", "arrival_offset_us", "input_tokens", "text", "output_tokens" });
        const auto id = string_value(item.at("id"));
        if (!ids.insert(id).second) { throw std::invalid_argument("duplicate request id: " + id); }

        if (item.contains("input_tokens") == item.contains("text")) {
            throw std::invalid_argument("request must contain exactly one of input_tokens and text");
        }

        Request request{ id,
                         integer(item.at("arrival_offset_us"), 0, std::numeric_limits<std::int64_t>::max() / 1'000) * 1'000,
                         {},
                         positive(item.at("output_tokens")) };
        if (item.contains("text")) { request.input = string_value(item.at("text")); }
        else {
            const auto & tokens = item.at("input_tokens");
            if (!tokens.is_array() || tokens.empty()) { throw std::invalid_argument("input_tokens must be a nonempty array"); }

            std::vector<std::uint32_t> input;
            for (const auto & token : tokens) { input.push_back(static_cast<std::uint32_t>(integer(token, 0, std::numeric_limits<std::uint32_t>::max()))); }

            request.input = std::move(input);
        }

        result.push_back(std::move(request));
    }

    return result;
}

FrameworkConfiguration read_configuration(const Path & path, Framework framework) {
    const auto data = read_json(path);
    fields(data, { "parameters", "model_assets" });
    const auto & p = data.at("parameters");
    const auto assets = data.value("model_assets", Json::object());

    if (framework == Framework::SGLang) {
        fields(assets, {});
        return SGLangConfiguration::from_parameters(p);
    }

    fields(p, { "backend_type", "tp", "max_batch_size", "chunk_size", "cpu_infer", "cache_lens", "sched_strategy", "use_cuda_graph" });
    fields(assets, { "gguf_path", "optimize_config_path" });

    return KTransformersConfiguration{ string_value(p.at("backend_type")),
                                       positive(p.at("tp")),
                                       positive(p.at("max_batch_size")),
                                       positive(p.at("chunk_size")),
                                       positive(p.at("cpu_infer")),
                                       positive(p.at("cache_lens")),
                                       string_value(p.at("sched_strategy")),
                                       p.at("use_cuda_graph").get<bool>(),
                                       resolve_path(path, assets.at("gguf_path")),
                                       resolve_path(path, assets.at("optimize_config_path")) };
}

CollectionPermission read_permission(const Json & value) {
    fields(value, { "allow_run", "max_wall_time_sec" });
    const bool allow_run = value.value("allow_run", false);
    const auto budget = integer(value.value("max_wall_time_sec", Json(0)), 0, std::numeric_limits<std::int64_t>::max() / 1'000'000'000);
    if (allow_run && budget == 0) { throw std::invalid_argument("enabled collection requires a positive max_wall_time_sec"); }

    return { allow_run, budget };
}

} // namespace

SGLangConfiguration SGLangConfiguration::from_parameters(const Json & p) {
    fields(p,
           { "device",
             "tp_size",
             "attention_backend",
             "max_running_requests",
             "max_total_tokens",
             "chunked_prefill_size",
             "max_prefill_tokens",
             "schedule_policy",
             "disable_radix_cache",
             "disable_overlap_schedule",
             "disable_cuda_graph",
             "enable_mixed_chunk",
             "enable_hierarchical_cache" });

    const auto chunk_size = integer(p.at("chunked_prefill_size"), -1, std::numeric_limits<std::int32_t>::max());
    if (chunk_size == 0) { throw std::invalid_argument("chunked_prefill_size is -1 or a positive integer"); }

    return SGLangConfiguration{ string_value(p.at("device")),
                                positive(p.at("tp_size")),
                                string_value(p.at("attention_backend")),
                                positive(p.at("max_running_requests")),
                                positive(p.at("max_total_tokens")),
                                static_cast<std::int32_t>(chunk_size),
                                positive(p.at("max_prefill_tokens")),
                                string_value(p.at("schedule_policy")),
                                p.at("disable_radix_cache").get<bool>(),
                                p.at("disable_overlap_schedule").get<bool>(),
                                p.at("disable_cuda_graph").get<bool>(),
                                p.at("enable_mixed_chunk").get<bool>(),
                                p.at("enable_hierarchical_cache").get<bool>() };
}

nlohmann::json SGLangConfiguration::parameters() const {
    return {
        {                    "device",                    device },
        {                   "tp_size",                   tp_size },
        {         "attention_backend",         attention_backend },
        {      "max_running_requests",      max_running_requests },
        {          "max_total_tokens",          max_total_tokens },
        {      "chunked_prefill_size",      chunked_prefill_size },
        {        "max_prefill_tokens",        max_prefill_tokens },
        {           "schedule_policy",           schedule_policy },
        {       "disable_radix_cache",       disable_radix_cache },
        {  "disable_overlap_schedule",  disable_overlap_schedule },
        {        "disable_cuda_graph",        disable_cuda_graph },
        {        "enable_mixed_chunk",        enable_mixed_chunk },
        { "enable_hierarchical_cache", enable_hierarchical_cache }
    };
}

Project ProjectReader::read(const std::filesystem::path & path) const {
    Project project;
    project.path = std::filesystem::absolute(path).lexically_normal();
    const auto data = read_json(project.path);
    fields(data, { "model", "configurations", "request_workloads", "simulation", "initial_state", "collection", "assets", "output" });

    const auto & model = data.at("model");
    fields(model, { "path", "tokenizer_path" });
    project.model_path = resolve_path(project.path, model.at("path"));
    project.tokenizer_path = model.contains("tokenizer_path") ? resolve_path(project.path, model.at("tokenizer_path")) : project.model_path;

    const auto & configurations = data.at("configurations");
    if (!configurations.is_array() || configurations.empty()) throw std::invalid_argument("configurations must be a nonempty array");

    std::set<std::string> configuration_ids;
    for (const auto & item : configurations) {
        fields(item, { "id", "framework", "environment", "config" });
        const auto id = string_value(item.at("id"));
        if (!configuration_ids.insert(id).second) throw std::invalid_argument("duplicate configuration id: " + id);
        const auto framework = string_value(item.at("framework"));
        if (framework != "sglang" && framework != "ktransformers") throw std::invalid_argument("unknown framework: " + framework);
        const auto kind = framework == "sglang" ? Framework::SGLang : Framework::KTransformers;
        const auto config_path = resolve_path(project.path, item.at("config"));

        const auto & environment = item.at("environment");
        fields(environment, { "runtime", "service", "devices" });
        if (environment.value("runtime", std::string("docker")) != "docker") throw std::invalid_argument("environment.runtime must be docker");
        Environment deployment{ string_value(environment.at("service")), {} };
        if (deployment.service != framework + "-profile") throw std::invalid_argument("environment.service differs from framework");
        const auto & devices = environment.at("devices");
        if (!devices.is_array() || devices.empty()) throw std::invalid_argument("environment.devices must be a nonempty array");
        std::set<std::uint32_t> device_ids;
        for (const auto & value : devices) {
            const auto device = static_cast<std::uint32_t>(integer(value, 0, std::numeric_limits<std::uint32_t>::max()));
            if (!device_ids.insert(device).second) throw std::invalid_argument("duplicate device number");
            deployment.devices.push_back(device);
        }

        project.configurations.push_back({ id, kind, std::move(deployment), config_path, read_configuration(config_path, kind) });
    }

    const auto & workloads = data.at("request_workloads");
    if (!workloads.is_array() || workloads.empty()) throw std::invalid_argument("request_workloads must be a nonempty array");
    std::set<std::string> workload_ids;
    for (const auto & item : workloads) {
        fields(item, { "id", "path" });
        const auto id = string_value(item.at("id"));
        if (!workload_ids.insert(id).second) throw std::invalid_argument("duplicate workload id: " + id);
        const auto workload_path = resolve_path(project.path, item.at("path"));
        project.request_workloads.push_back({ id, workload_path, read_workload(workload_path) });
    }

    if (data.contains("simulation")) {
        const auto & simulation = data.at("simulation");
        fields(simulation, { "controller", "scope" });
        project.controller = simulation.value("controller", std::string("lightweight"));
        const auto scope = simulation.value("scope", std::string("modeled"));
        if (scope != "modeled" && scope != "http_e2e") throw std::invalid_argument("scope must be modeled or http_e2e");
        project.scope = scope == "modeled" ? Scope::Modeled : Scope::HttpE2E;
    }
    if (data.contains("initial_state")) {
        const auto & initial = data.at("initial_state");
        fields(initial, { "cache" });
        const auto cache = initial.value("cache", std::string("cold"));
        if (cache != "cold" && cache != "warm") throw std::invalid_argument("cache must be cold or warm");
        project.initial_cache = cache == "cold" ? CacheState::Cold : CacheState::Warm;
    }
    if (data.contains("collection")) {
        const auto & collection = data.at("collection");
        fields(collection, { "base", "performance" });
        if (collection.contains("base")) project.base_collection = read_permission(collection.at("base"));
        if (collection.contains("performance")) project.performance_collection = read_permission(collection.at("performance"));
    }
    if (data.contains("assets")) {
        const auto & assets = data.at("assets");
        fields(assets, { "templates", "performance_manifests" });
        const auto templates = assets.value("templates", Json::array());
        if (!templates.is_array()) throw std::invalid_argument("assets.templates must be an array");
        std::set<Path> manifests;
        for (const auto & item : templates) {
            fields(item, { "manifest", "node_durations" });
            TemplateInput input{ resolve_path(project.path, item.at("manifest")), std::nullopt };
            if (!manifests.insert(input.manifest).second) throw std::invalid_argument("duplicate template manifest");
            if (item.contains("node_durations") && !item.at("node_durations").is_null())
                input.node_durations = resolve_path(project.path, item.at("node_durations"));
            project.templates.push_back(std::move(input));
        }
        const auto performances = assets.value("performance_manifests", Json::array());
        if (!performances.is_array()) throw std::invalid_argument("performance_manifests must be an array");
        for (const auto & manifest : performances) project.performance_manifests.push_back(resolve_path(project.path, manifest));
    }

    const auto & output = data.at("output");
    fields(output, { "directory", "diagnostics" });
    project.output_directory = resolve_path(project.path, output.at("directory"));
    project.diagnostics = output.value("diagnostics", false);
    return project;
}

const Candidate & Project::configuration(std::string_view id) const {
    for (const auto & candidate : configurations) {
        if (candidate.id == id) return candidate;
    }
    throw std::invalid_argument("unknown configuration: " + std::string(id));
}

const RequestWorkload & Project::workload(std::string_view id) const {
    for (const auto & item : request_workloads) {
        if (item.id == id) return item;
    }
    throw std::invalid_argument("unknown workload: " + std::string(id));
}

} // namespace markov
