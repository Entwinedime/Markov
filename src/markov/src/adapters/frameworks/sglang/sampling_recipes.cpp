#include "markov/adapters/frameworks/sglang/sampling_recipes.hpp"

#include "markov/adapters/frameworks/sglang/bindings.hpp"
#include "markov/contracts/json_input.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
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

std::uint32_t measurement_count(const Json & value) { return static_cast<std::uint32_t>(input::integer(value, 0, std::numeric_limits<std::uint32_t>::max())); }

} // namespace

nlohmann::json SGLangSamplingRecipes::sampling_requests(const SGLangConfiguration & configuration, std::span<const Request> prompt_sources,
                                                        const Json & requirements) const {
    if (!requirements.is_array()) throw std::invalid_argument("performance sampling requirements must be an array");

    Json points = Json::array();
    Json assignments = Json::array();
    Json unresolved = Json::array();
    std::map<std::string, std::expected<std::size_t, std::string>> planned;
    std::map<std::string, std::size_t> equivalent_points;

    for (std::size_t index = 0; index < requirements.size(); ++index) {
        const auto & requirement = requirements[index];
        if (requirement.at("coverage") != "needs_data") {
            unresolved.push_back({
                { "requirement_index",                                            index },
                {            "reason", "performance_requirement_not_ready_for_sampling" }
            });
            continue;
        }
        const auto & features = requirement.at("object").at("features");
        const auto operands = features.find("operand_work");
        const auto * filter = operands != features.end() && operands->contains("batch_filter") ? &operands->at("batch_filter") : nullptr;
        const auto * pool = operands != features.end() && operands->contains("request_pool") ? &operands->at("request_pool") : nullptr;
        Json point;
        if (filter) {
            // Filter costs depend on selection, not target request history.
            // A short native prefill triggers the same helper without restoring KV state.
            point = {
                {  "batch_filter", *filter },
                { "prompt_tokens",       1 }
            };
        }
        else {
            const auto origin = requirement.find("work_origin");
            if (origin == requirement.end() || origin->is_null()) {
                unresolved.push_back({
                    { "requirement_index",                             index },
                    {            "reason", "sampling_work_origin_unresolved" }
                });
                continue;
            }

            const auto phase = origin->at("phase").get<std::string>();
            std::string reason;
            if ((phase != "prefill" && phase != "decode") || origin->at("requests").empty()) reason = "sampling_requires_nonempty_prefill_or_decode";

            Json pool_slots = Json::array();
            if (pool) {
                if (pool->contains("slots")) pool_slots = pool->at("slots");
                else
                    for (const auto & member : pool->at("members")) pool_slots.push_back(member.at("slot"));
                if (pool_slots.size() != origin->at("requests").size()) reason = "sampling_request_pool_membership_unresolved";
            }

            std::vector<std::tuple<bool, Json, Json>> members;
            std::size_t finished = 0;
            for (const auto & request : origin->at("requests")) {
                if (!reason.empty()) break;
                const auto request_id = request.at("request_id").get<std::size_t>();
                if (request_id >= prompt_sources.size()) {
                    reason = "sampling_work_origin_request_outside_workload";
                    break;
                }
                const auto & source = prompt_sources[request_id];
                const auto * tokens = std::get_if<std::vector<std::uint32_t>>(&source.input);
                if (!tokens) {
                    reason = "sampling_prompt_tokens_unresolved";
                    break;
                }

                const auto input = request.at("input_tokens").get<std::uint32_t>();
                const auto context = request.at("context_tokens").get<std::uint32_t>();
                const auto generated = request.at("generated_tokens").get<std::uint32_t>();
                const auto prompt = static_cast<std::uint64_t>(tokens->size());
                if ((phase == "prefill" && (input != prompt || context != 0 || generated != 0))
                    || (phase == "decode" && (input != 1 || generated == 0 || static_cast<std::uint64_t>(context) + 1 != prompt + generated))) {
                    reason = "sampling_work_origin_outside_cold_dense_request_progression";
                    break;
                }

                const bool finishes = request.at("finishes").get<bool>();
                finished += finishes;
                members.emplace_back(finishes,
                                     Json{
                                         {   "input_tokens",   input },
                                         { "context_tokens", context },
                                         {  "prompt_tokens",  prompt }
                },
                                     pool ? pool_slots.at(members.size()) : Json(nullptr));
            }
            if (!reason.empty()) {
                unresolved.push_back({
                    { "requirement_index",             index },
                    {            "reason", std::move(reason) }
                });
                continue;
            }

            // The existing recipe names the first K members as finished. It still
            // lets the native scheduler own batching; real named features decide coverage.
            std::stable_sort(members.begin(), members.end(), [](const auto & left, const auto & right) { return std::get<0>(left) > std::get<0>(right); });
            Json requests = Json::array();
            pool_slots = Json::array();
            for (auto & [finishes, request, slot] : members) {
                requests.push_back(std::move(request));
                if (pool) pool_slots.push_back(std::move(slot));
            }
            point = Json{
                {                  "phase",               phase },
                {               "requests", std::move(requests) },
                { "finished_request_count",            finished }
            };
            if (pool) point["pool_slots"] = std::move(pool_slots);
        }

        const auto key = point.dump();
        auto planned_point = planned.find(key);
        if (planned_point == planned.end()) {
            std::expected<std::size_t, std::string> result = points.size();
            try {
                // Reuse actual request construction for legal prompt, capacity
                // and context checks instead of maintaining another native rule set.
                const auto recipes = workloads(configuration, prompt_sources, Json::array({ point }));
                if (!point.contains("batch_filter")) {
                    auto domain = point;
                    domain.erase("pool_slots");
                    const auto recipe_key = Json::array({ std::move(domain), recipes.at(0).at("requests") }).dump();
                    const auto [existing, inserted] = equivalent_points.try_emplace(recipe_key, points.size());
                    if (!inserted) {
                        auto & retained = points.at(existing->second);
                        if (!point.contains("pool_slots") || !retained.contains("pool_slots") || point.at("pool_slots") == retained.at("pool_slots")) {
                            // Keep the stable index for every assignment, while
                            // preserving the stronger slot observation requirement.
                            result = existing->second;
                            if (point.contains("pool_slots")) retained = std::move(point);
                        }
                    }
                }
                if (*result == points.size()) points.push_back(std::move(point));
            }
            catch (const std::invalid_argument & error) {
                result = std::unexpected(error.what());
            }
            planned_point = planned.emplace(key, std::move(result)).first;
        }

        if (planned_point->second)
            assignments.push_back({
                { "requirement_index",                      index },
                {     "point_indices", { *planned_point->second } },
                {            "status",          "recipe_proposed" }
            });
        else
            unresolved.push_back({
                { "requirement_index",                         index },
                {            "reason", planned_point->second.error() }
            });
    }

    return {
        { "measurement_requests",      std::move(points) },
        {          "assignments", std::move(assignments) },
        {           "unresolved",  std::move(unresolved) }
    };
}

nlohmann::json SGLangSamplingRecipes::workloads(const SGLangConfiguration & configuration, std::span<const Request> prompt_sources,
                                                const Json & measurement_requests) const {
    if (!measurement_requests.is_array() || measurement_requests.empty())
        throw std::invalid_argument("performance sampling requires explicit nonempty measurement requests");

    // Sampling observes the same serving policy qualified for conditional execution.
    if (!SGLangBindings{}.control_limits(configuration)) {
        throw std::invalid_argument("sampling requires the declared FCFS, separate, unchunked, non-overlap Ascend path");
    }

    std::map<std::size_t, const Request *> representatives;
    for (const auto & request : prompt_sources) {
        const auto * tokens = std::get_if<std::vector<std::uint32_t>>(&request.input);
        if (!tokens) throw std::invalid_argument("performance sampling requires resolved token inputs");
        representatives.try_emplace(tokens->size(), &request);
    }

    Json grid = Json::array();
    for (const auto & domain : measurement_requests) {
        if (domain.contains("batch_filter")) {
            input::fields(domain, { "batch_filter", "prompt_tokens" });
            const auto & filter = domain.at("batch_filter");
            input::fields(filter, { "input_count", "retained_indices" });
            if (!filter.at("retained_indices").is_array()) throw std::invalid_argument("filter retained indices must be an array");

            const auto count = measurement_count(filter.at("input_count"));
            const auto prompt = input::positive(domain.at("prompt_tokens"));
            std::vector<std::uint32_t> retained;
            for (const auto & index : filter.at("retained_indices")) retained.push_back(measurement_count(index));
            if (count > configuration.max_running_requests || retained.empty() || retained.size() >= count || retained.back() >= count
                || !std::ranges::is_sorted(retained) || std::ranges::adjacent_find(retained) != retained.end())
                throw std::invalid_argument("filter sampling requires a nonempty partial selection in native member order");

            const auto source = representatives.lower_bound(prompt);
            if (source == representatives.end() || static_cast<std::uint64_t>(count) * prompt > configuration.max_prefill_tokens
                || count > configuration.max_total_tokens / (static_cast<std::uint64_t>(prompt) + retained.size() + 1))
                throw std::invalid_argument("filter sampling lacks a prompt source or sufficient batch capacity");

            const auto & representative = *source->second;
            const auto & tokens = std::get<std::vector<std::uint32_t>>(representative.input);
            Json recipe = Json::array();
            for (std::uint32_t slot = 0; slot < count; ++slot) {
                // Preserve the first selection, then let retained members
                // finish in order within this same native request lifecycle.
                const auto kept = std::ranges::lower_bound(retained, slot);
                const auto output = kept != retained.end() && *kept == slot ? 2U + static_cast<std::uint32_t>(kept - retained.begin()) : 1U;
                recipe.push_back({
                    { "input_tokens", std::vector<std::uint32_t>(tokens.begin(), tokens.begin() + prompt) },
                    { "output_tokens", output },
                    { "release_after_tokens", 0 },
                    { "prompt_source", { { "request_id", representative.id }, { "available_tokens", tokens.size() }, { "selected_tokens", prompt } } }
                });
            }
            grid.push_back({
                {       "id",       grid.size() },
                { "requests", std::move(recipe) }
            });
            continue;
        }

        input::fields(domain, { "phase", "requests", "finished_request_count", "pool_slots" });

        const auto phase = domain.at("phase").get<std::string>();
        const auto & requests = domain.at("requests");
        const auto finished = measurement_count(domain.at("finished_request_count"));
        if ((phase != "prefill" && phase != "decode") || !requests.is_array() || requests.empty() || requests.size() > configuration.max_running_requests
            || finished > requests.size()) {
            throw std::invalid_argument("measurement request exceeds the declared phase or batch domain");
        }

        std::vector<std::uint32_t> pool_slots;
        if (domain.contains("pool_slots")) {
            const auto & slots = domain.at("pool_slots");
            if (!slots.is_array() || slots.size() != requests.size()) throw std::invalid_argument("pool slots must match the requested members");
            for (const auto & slot : slots) pool_slots.push_back(input::positive(slot));
            if (pool_slots.back() > configuration.max_running_requests || !std::ranges::is_sorted(pool_slots)
                || std::ranges::adjacent_find(pool_slots) != pool_slots.end())
                throw std::invalid_argument("pool slots must follow the native ascending reservation order within capacity");
            if (phase == "prefill" && pool_slots.back() != requests.size())
                throw std::invalid_argument("prefill slot probes would change the requested batch shape");
        }

        std::vector<std::uint32_t> generated;
        std::uint64_t prefill_prefix_tokens = 0;
        for (const auto & request : requests) {
            input::fields(request, { "input_tokens", "context_tokens", "prompt_tokens" });

            const auto prompt = measurement_count(request.at("prompt_tokens"));
            const auto context = measurement_count(request.at("context_tokens"));
            const auto input = measurement_count(request.at("input_tokens"));
            if (prompt == 0 || representatives.lower_bound(prompt) == representatives.end()
                || static_cast<std::uint64_t>(std::max(prompt, context)) + 2 > configuration.max_total_tokens
                || (phase == "prefill" && (context != 0 || input != prompt)) || (phase == "decode" && (input != 1 || context < prompt))) {
                throw std::invalid_argument("measurement work lacks a prompt source or a legal cold dense context");
            }

            if (phase == "prefill") {
                // Native admission may exhaust its budget on the last member.
                // An exhausted prefix cannot admit another, even before page rounding.
                if (prefill_prefix_tokens >= configuration.max_prefill_tokens)
                    throw std::invalid_argument("prefill measurement members cannot fit the native admission budget");
                prefill_prefix_tokens += prompt;
            }

            generated.push_back(phase == "prefill" ? 0 : context - prompt + 1);
        }

        const auto maximum_generated = *std::max_element(generated.begin(), generated.end());
        if (!pool_slots.empty() && std::ranges::any_of(generated, [maximum_generated](auto value) { return value != maximum_generated; }))
            throw std::invalid_argument("pool slot probes require simultaneously submitted payload requests");

        Json recipe = Json::array();
        for (std::size_t slot = 0; slot < requests.size(); ++slot) {
            const auto prompt = requests[slot].at("prompt_tokens").get<std::uint32_t>();
            const auto & representative = *representatives.lower_bound(prompt)->second;
            const auto & tokens = std::get<std::vector<std::uint32_t>>(representative.input);
            // Use a real token prefix from the smallest sufficient source. The
            // request workload supplies content, not a serving arrival sequence.
            recipe.push_back({
                { "input_tokens", std::vector<std::uint32_t>(tokens.begin(), tokens.begin() + prompt) },
                { "output_tokens", generated[slot] + (slot < finished ? 1 : 2) },
                { "release_after_tokens", maximum_generated - generated[slot] },
                { "prompt_source", { { "request_id", representative.id }, { "available_tokens", tokens.size() }, { "selected_tokens", prompt } } }
            });
        }

        if (!pool_slots.empty()) {
            const auto padding = pool_slots.back() - requests.size();
            std::uint64_t prefill_tokens = padding;
            std::uint64_t reserved_tokens = 2 * padding;
            for (const auto & request : recipe) {
                const auto prompt = request.at("input_tokens").size();
                prefill_tokens += prompt;
                reserved_tokens += prompt + request.at("output_tokens").get<std::uint32_t>();
            }
            if ((phase == "decode" && prefill_tokens > configuration.max_prefill_tokens) || reserved_tokens > configuration.max_total_tokens)
                throw std::invalid_argument("pool slot probes and payload exceed the declared token capacity");

            if (padding != 0) {
                const auto source = representatives.lower_bound(1);
                if (source == representatives.end()) throw std::invalid_argument("pool slot probes require a nonempty token source");
                const auto & representative = *source->second;
                const auto & tokens = std::get<std::vector<std::uint32_t>>(representative.input);
                Json cohort = Json::array();
                std::size_t payload = 0;
                // These are real requests, not writes to native allocator state.
                // Their completion releases the lower reservations naturally.
                for (std::size_t slot = 1; slot <= pool_slots.back(); ++slot) {
                    if (payload < pool_slots.size() && slot == pool_slots[payload]) {
                        cohort.push_back(std::move(recipe[payload]));
                        ++payload;
                    }
                    else
                        cohort.push_back({
                            {         "input_tokens",                                                             std::vector<std::uint32_t>{ tokens.front() } },
                            {        "output_tokens",                                                                                                        1 },
                            { "release_after_tokens",                                                                                                        0 },
                            {        "prompt_source", { { "request_id", representative.id }, { "available_tokens", tokens.size() }, { "selected_tokens", 1 } } }
                        });
                }
                recipe = std::move(cohort);
            }
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
        const auto manifest = input::read_json(path);
        if (manifest.at("role") != "performance") throw std::invalid_argument("communication work domains require role=performance assets");
        if (manifest.at("measurement_kind") != "native_device_work") continue;
        if (manifest.at("framework") != "sglang") throw std::invalid_argument("communication work domains require the declared SGLang deployment");

        const auto prepared = input::read_json(input::resolve_path(path, manifest.at("input")));
        if (prepared.at("configuration").at("tp_size") != configuration.tp_size || prepared.at("environment").at("devices") != environment.devices) {
            throw std::invalid_argument("communication work domain rank/device deployment differs from the project");
        }

        const auto measurements = input::read_json(input::resolve_path(path, manifest.at("device_measurements")));
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
