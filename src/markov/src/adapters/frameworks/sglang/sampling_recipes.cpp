#include "markov/adapters/frameworks/sglang/sampling_recipes.hpp"

#include "markov/adapters/frameworks/sglang/bindings.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace markov {
namespace {

using Json = nlohmann::json;

std::uint32_t measurement_count(const Json & value) {
    if (!value.is_number_integer() || value < 0 || value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("measurement work counts must be nonnegative uint32 integers");
    }

    return value.get<std::uint32_t>();
}

} // namespace

nlohmann::json SGLangSamplingRecipes::workloads(const SGLangConfiguration & configuration, std::span<const Request> prompt_sources,
                                                const Json & measurement_requests) const {
    if (!measurement_requests.is_array() || measurement_requests.empty())
        throw std::invalid_argument("CPU/device sampling requires explicit nonempty measurement requests");

    // Sampling observes the same serving policy qualified for conditional execution.
    if (!SGLangBindings{}.control_limits(configuration)) {
        throw std::invalid_argument("sampling requires the declared FCFS, separate, unchunked, non-overlap Ascend path");
    }

    std::map<std::size_t, const Request *> representatives;
    for (const auto & request : prompt_sources) {
        const auto * tokens = std::get_if<std::vector<std::uint32_t>>(&request.input);
        if (!tokens) throw std::invalid_argument("CPU/device sampling requires resolved token inputs");
        representatives.try_emplace(tokens->size(), &request);
    }

    Json grid = Json::array();
    for (const auto & domain : measurement_requests) {
        if (!domain.is_object() || domain.size() != 3 || !domain.contains("phase") || !domain.contains("requests")
            || !domain.contains("finished_request_count")) {
            throw std::invalid_argument("measurement requests require phase, requests and finished_request_count");
        }

        const auto phase = domain.at("phase").get<std::string>();
        const auto & requests = domain.at("requests");
        const auto finished = measurement_count(domain.at("finished_request_count"));
        if ((phase != "prefill" && phase != "decode") || !requests.is_array() || requests.empty() || requests.size() > configuration.max_running_requests
            || finished > requests.size()) {
            throw std::invalid_argument("measurement request exceeds the declared phase or batch domain");
        }

        std::vector<std::uint32_t> generated;
        for (const auto & request : requests) {
            if (!request.is_object() || request.size() != 3 || !request.contains("input_tokens") || !request.contains("context_tokens")
                || !request.contains("prompt_tokens")) {
                throw std::invalid_argument("measurement work requires input_tokens, context_tokens and prompt_tokens");
            }

            const auto prompt = measurement_count(request.at("prompt_tokens"));
            const auto context = measurement_count(request.at("context_tokens"));
            const auto input = measurement_count(request.at("input_tokens"));
            if (!representatives.contains(prompt) || static_cast<std::uint64_t>(context) + 2 > configuration.max_total_tokens
                || (phase == "prefill" && (context != 0 || input != prompt)) || (phase == "decode" && (input != 1 || context < prompt))) {
                throw std::invalid_argument("measurement work lacks a prompt source or a legal cold dense context");
            }

            generated.push_back(phase == "prefill" ? 0 : context - prompt + 1);
        }

        const auto maximum_generated = *std::max_element(generated.begin(), generated.end());
        Json recipe = Json::array();
        for (std::size_t slot = 0; slot < requests.size(); ++slot) {
            const auto & representative = *representatives.at(requests[slot].at("prompt_tokens").get<std::uint32_t>());
            recipe.push_back({
                {         "input_tokens", std::get<std::vector<std::uint32_t>>(representative.input) },
                {        "output_tokens",                generated[slot] + (slot < finished ? 1 : 2) },
                { "release_after_tokens",                        maximum_generated - generated[slot] }
            });
        }

        // Earlier requests form the native running batch first. Delayed requests
        // are released by real token progress, without copying target arrivals.
        std::stable_sort(recipe.begin(), recipe.end(), [](const Json & a, const Json & b) {
            return a.at("release_after_tokens") < b.at("release_after_tokens");
        });
        grid.push_back({
            {       "id",       grid.size() },
            { "requests", std::move(recipe) }
        });
    }

    return grid;
}

nlohmann::json SGLangSamplingRecipes::collectives(const SGLangConfiguration & configuration, const Environment & environment,
                                                  std::span<const std::filesystem::path> performance_manifests) const {
    if (configuration.device != "npu" || environment.devices.size() != configuration.tp_size || configuration.tp_size < 2) {
        throw std::invalid_argument("communication sampling requires an Ascend deployment with one declared device per rank and TP >= 2");
    }

    Json grid = Json::array();
    std::set<std::tuple<std::string, std::uint64_t, std::string, std::string>> conditions;
    for (const auto & path : performance_manifests) {
        const auto manifest = Json::parse(std::ifstream(path));
        if (manifest.at("role") != "performance") throw std::invalid_argument("communication work domains require role=performance assets");
        if (manifest.at("measurement_kind") != "native_device_work") continue;
        if (manifest.at("framework") != "sglang") throw std::invalid_argument("communication work domains require the declared SGLang deployment");

        const auto input = Json::parse(std::ifstream(path.parent_path() / manifest.at("input").get<std::string>()));
        if (input.at("configuration").at("tp_size") != configuration.tp_size || input.at("environment").at("devices") != environment.devices) {
            throw std::invalid_argument("communication work domain rank/device deployment differs from the project");
        }

        const auto measurements = Json::parse(std::ifstream(path.parent_path() / manifest.at("device_measurements").get<std::string>()));
        const auto & descriptors = measurements.at("collective_descriptors");
        for (std::size_t index = 0; index < descriptors.size(); ++index) {
            const auto & descriptor = descriptors[index];
            if (descriptor.at("stage") != "measurement") continue;

            const auto operation = descriptor.at("operation").get<std::string>();
            const auto count = descriptor.at("element_count").get<std::uint64_t>();
            const auto dtype = descriptor.at("data_type").get<std::string>();
            const auto algorithm = descriptor.at("algorithm").get<std::string>();
            if ((operation != "hcom_allReduce" && operation != "hcom_allGather") || dtype != "BFP16" || count == 0) {
                throw std::invalid_argument("communication sampling currently supports nonempty BFP16 allReduce/allGather work");
            }
            if (!conditions.emplace(operation, count, dtype, algorithm).second) continue;

            grid.push_back({
                {            "id",                                                      grid.size() },
                {     "operation",                                                        operation },
                { "element_count",                                                            count },
                {     "data_type",                                                            dtype },
                {     "algorithm",                                                        algorithm },
                {        "source", { { "manifest", path.string() }, { "descriptor_index", index } } }
            });
        }
    }

    return grid;
}

} // namespace markov
