#include "markov/workflow/project_reader.hpp"

#include "markov/adapters/collection_adapter.hpp"
#include "markov/contracts/json_input.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace markov {
namespace {

using Json = nlohmann::json;
using Path = std::filesystem::path;
using input::fields;
using input::integer;
using input::positive;
using input::read_json;
using input::resolve_path;
using input::string_value;

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

CollectionPermission read_permission(const Json & value) {
    fields(value, { "allow_run", "max_wall_time_sec" });
    const bool allow_run = value.value("allow_run", false);
    const auto budget = integer(value.value("max_wall_time_sec", Json(0)), 0, std::numeric_limits<std::int64_t>::max() / 1'000'000'000);
    if (allow_run && budget == 0) { throw std::invalid_argument("enabled collection requires a positive max_wall_time_sec"); }

    return { allow_run, budget };
}

} // namespace

Project ProjectReader::read(const std::filesystem::path & path) const {
    Project project;
    project.path = std::filesystem::absolute(path).lexically_normal();
    const auto data = read_json(project.path);
    fields(data, { "model", "configurations", "request_workloads", "collection", "assets", "output" });

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
        const auto adapter = std::ranges::find_if(adapters_, [&](const CollectionAdapter * item) { return item->name() == framework; });
        if (adapter == adapters_.end()) throw std::invalid_argument("unknown framework: " + framework);
        const auto & binding = **adapter;
        const auto kind = binding.framework();
        const auto config_path = resolve_path(project.path, item.at("config"));

        const auto & environment = item.at("environment");
        fields(environment, { "runtime", "service", "devices" });
        if (environment.value("runtime", std::string("docker")) != "docker") throw std::invalid_argument("environment.runtime must be docker");
        Environment deployment{ string_value(environment.at("service")), {} };
        if (deployment.service != binding.service()) throw std::invalid_argument("environment.service differs from framework");
        const auto & devices = environment.at("devices");
        if (!devices.is_array() || devices.empty()) throw std::invalid_argument("environment.devices must be a nonempty array");
        std::set<std::uint32_t> device_ids;
        for (const auto & value : devices) {
            const auto device = static_cast<std::uint32_t>(integer(value, 0, std::numeric_limits<std::uint32_t>::max()));
            if (!device_ids.insert(device).second) throw std::invalid_argument("duplicate device number");
            deployment.devices.push_back(device);
        }

        project.configurations.push_back({ id, kind, std::move(deployment), config_path, binding.read_configuration(config_path, read_json(config_path)) });
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

    if (data.contains("collection")) {
        const auto & collection = data.at("collection");
        fields(collection, { "base", "performance" });
        if (collection.contains("base")) project.base_collection = read_permission(collection.at("base"));
        if (collection.contains("performance")) project.performance_collection = read_permission(collection.at("performance"));
    }
    if (data.contains("assets")) {
        const auto & assets = data.at("assets");
        fields(assets, { "templates", "performance_manifests", "cost_models" });
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
        const auto models = assets.value("cost_models", Json::array());
        if (!models.is_array()) throw std::invalid_argument("cost_models must be an array");
        for (const auto & model : models) project.cost_models.push_back(resolve_path(project.path, model));
    }

    const auto & output = data.at("output");
    fields(output, { "directory", "diagnostics" });
    project.output_directory = resolve_path(project.path, output.at("directory"));
    project.diagnostics = output.value("diagnostics", false);
    return project;
}

} // namespace markov
