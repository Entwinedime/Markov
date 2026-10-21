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
                                                        const Json & requirements, std::span<const std::uint32_t> observed_ranks) const {
    Json points = Json::array();
    Json grid = Json::array();
    Json assignments = Json::array();
    Json unresolved = Json::array();
    std::map<std::string, std::expected<std::size_t, std::string>> planned;
    std::map<std::string, std::size_t> equivalent_points;

    // The workflow selects uncovered demands; this adapter decides native reachability.
    for (std::size_t index = 0; index < requirements.size(); ++index) {
        const auto & requirement = requirements[index];
        // Local shard execution substitutes this whole operation. Neither its
        // helper timing nor a physical task sample has the target boundary.
        if (requirement.at("object").at("kind") == "collective") {
            const bool host = requirement.at("object").at("features").at("execution") == "host_synchronous";
            unresolved.push_back({
                { "requirement_index",                                                                                                                          index },
                {            "reason", host ? "host_message_service_requires_native_group_measurement" : "whole_collective_service_requires_native_group_measurement" }
            });
            continue;
        }
        const auto & features = requirement.at("object").at("features");
        if (requirement.at("object").at("kind") == "cpu"
            && std::ranges::find(observed_ranks, features.at("rank").get<std::uint32_t>()) == observed_ranks.end()) {
            unresolved.push_back({
                { "requirement_index",                                     index },
                {            "reason", "cpu_role_not_available_in_local_capture" }
            });
            continue;
        }
        const auto operands = features.find("operand_work");
        const auto * filter = operands != features.end() && operands->contains("batch_filter") ? &operands->at("batch_filter") : nullptr;
        const auto * pool = operands != features.end() && operands->contains("request_pool") ? &operands->at("request_pool") : nullptr;
        Json point;
        if (requirement.at("object").at("kind") == "cpu" && features.at("phase") == "idle") {
            point = {
                {    "phase",        "idle" },
                { "requests", Json::array() }
            };
        }
        else if (filter) {
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

            Json requests = Json::array();
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
                requests.push_back({
                    {   "input_tokens",                input },
                    { "context_tokens",              context },
                    {  "prompt_tokens",               prompt },
                    {  "output_tokens", source.output_tokens },
                    {       "finishes",             finishes }
                });
            }
            if (!reason.empty()) {
                unresolved.push_back({
                    { "requirement_index",             index },
                    {            "reason", std::move(reason) }
                });
                continue;
            }

            // Preserve native member coordinates and the declared output horizon.
            // One cohort also observes its later decode and partial-filter work.
            point = Json{
                {    "phase",               phase },
                { "requests", std::move(requests) }
            };
            if (pool) point["pool_slots"] = std::move(pool_slots);
            if (requirement.at("object").at("kind") == "cpu" && origin->at("step") == "schedule" && phase == "decode") {
                // Recreate the preceding selection with real short requests.
                // A survivor singleton never executes its member-removal work.
                const auto & filters = origin->at("batch_filters");
                for (auto selection = filters.rbegin(); selection != filters.rend(); ++selection) {
                    const auto retained = selection->at("retained_indices").size();
                    if (retained == point.at("requests").size() && retained < selection->at("input_count").get<std::size_t>()) {
                        point["preceding_filter"] = *selection;
                        if (const auto merge = origin->find("batch_merge"); merge != origin->end()) point["preceding_merge"] = *merge;
                        break;
                    }
                }
            }
        }

        const auto key = point.dump();
        auto planned_point = planned.find(key);
        if (planned_point == planned.end()) {
            std::expected<std::size_t, std::string> result = points.size();
            try {
                // Reuse actual request construction for legal prompt, capacity
                // and context checks instead of maintaining another native rule set.
                auto recipes = workloads(configuration, prompt_sources, Json::array({ point }));
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
                if (*result == points.size()) {
                    recipes.at(0)["id"] = points.size();
                    points.push_back(std::move(point));
                    grid.push_back(std::move(recipes.at(0)));
                }
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

    // Every declared request also supplies a bounded singleton lifecycle. These
    // cohorts cover later admission and completion in the same engine session;
    // they do not claim to enumerate timing-dependent multi-request batches.
    std::set<std::size_t> lifecycles;
    Json lifecycle_gaps = Json::array();
    std::map<std::string, std::size_t> cohorts;
    for (std::size_t index = 0; index < grid.size(); ++index) cohorts.try_emplace(grid.at(index).at("requests").dump(), index);
    for (const auto & source : prompt_sources) {
        const auto * tokens = std::get_if<std::vector<std::uint32_t>>(&source.input);
        if (!tokens) {
            lifecycle_gaps.push_back({
                { "request_id",                           source.id },
                {     "reason", "sampling_prompt_tokens_unresolved" }
            });
            continue;
        }

        Json member{
            {   "input_tokens",            tokens->size() },
            { "context_tokens",                         0 },
            {  "prompt_tokens",            tokens->size() },
            {  "output_tokens",      source.output_tokens },
            {       "finishes", source.output_tokens == 1 }
        };
        Json point{
            {    "phase",                          "prefill" },
            { "requests", Json::array({ std::move(member) }) }
        };
        try {
            auto recipe = workloads(configuration, prompt_sources, Json::array({ point }));
            const auto [entry, inserted] = cohorts.try_emplace(recipe.at(0).at("requests").dump(), points.size());
            if (inserted) {
                recipe.at(0)["id"] = points.size();
                points.push_back(std::move(point));
                grid.push_back(std::move(recipe.at(0)));
            }
            lifecycles.insert(entry->second);
        }
        catch (const std::invalid_argument & error) {
            lifecycle_gaps.push_back({
                { "request_id",    source.id },
                {     "reason", error.what() }
            });
        }
    }

    return {
        { "measurement_requests",         std::move(points) },
        {     "measurement_grid",           std::move(grid) },
        {          "assignments",    std::move(assignments) },
        {     "lifecycle_points",     std::move(lifecycles) },
        {       "lifecycle_gaps", std::move(lifecycle_gaps) },
        {           "unresolved",     std::move(unresolved) }
    };
}

nlohmann::json SGLangSamplingRecipes::make_request(const Request & source, std::uint32_t prompt_tokens, std::uint32_t output_tokens,
                                                   std::uint32_t release_after_tokens) {
    const auto & tokens = std::get<std::vector<std::uint32_t>>(source.input);
    return {
        { "input_tokens", std::vector<std::uint32_t>(tokens.begin(), tokens.begin() + prompt_tokens) },
        { "output_tokens", output_tokens },
        { "release_after_tokens", release_after_tokens },
        { "prompt_source", { { "request_id", source.id }, { "available_tokens", tokens.size() }, { "selected_tokens", prompt_tokens } } }
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
            Json recipe = Json::array();
            for (std::uint32_t slot = 0; slot < count; ++slot) {
                // Preserve the first selection, then let retained members
                // finish in order within this same native request lifecycle.
                const auto kept = std::ranges::lower_bound(retained, slot);
                const auto output = kept != retained.end() && *kept == slot ? 2U + static_cast<std::uint32_t>(kept - retained.begin()) : 1U;
                recipe.push_back(make_request(representative, prompt, output, 0));
            }
            grid.push_back({
                {       "id",       grid.size() },
                { "requests", std::move(recipe) }
            });
            continue;
        }

        input::fields(domain, { "phase", "requests", "pool_slots", "preceding_filter", "preceding_merge" });

        const auto phase = domain.at("phase").get<std::string>();
        const auto & requests = domain.at("requests");
        if (phase == "idle") {
            if (!requests.is_array() || !requests.empty() || domain.contains("pool_slots") || domain.contains("preceding_filter")
                || domain.contains("preceding_merge") || representatives.empty())
                throw std::invalid_argument("empty scheduling work requires no model members and an available prompt source");

            const auto & source = *representatives.begin()->second;
            const auto prompt = static_cast<std::uint32_t>(std::min<std::size_t>(std::get<std::vector<std::uint32_t>>(source.input).size(),
                                                                                 configuration.max_prefill_tokens / configuration.max_running_requests));
            if (prompt == 0) throw std::invalid_argument("empty scheduling probe cannot fill native request capacity within the input budget");

            // A full batch must finish in Decode while another request waits.
            // The native scheduler owns admission, cleanup and the empty return.
            Json recipe = Json::array();
            for (std::size_t slot = 0; slot < static_cast<std::size_t>(configuration.max_running_requests) + 1; ++slot)
                recipe.push_back(make_request(source, prompt, 2, 0));
            grid.push_back({
                {       "id",       grid.size() },
                { "requests", std::move(recipe) }
            });
            continue;
        }
        if ((phase != "prefill" && phase != "decode") || !requests.is_array() || requests.empty() || requests.size() > configuration.max_running_requests) {
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
        }

        std::size_t cohort_count = pool_slots.empty() ? requests.size() : pool_slots.back();
        const bool preceding_filter = domain.contains("preceding_filter");
        if (preceding_filter) {
            const auto & selection = domain.at("preceding_filter");
            input::fields(selection, { "input_count", "retained_indices" });
            cohort_count = input::positive(selection.at("input_count"));
            if (phase != "decode" || domain.contains("pool_slots") || cohort_count > configuration.max_running_requests
                || !selection.at("retained_indices").is_array() || selection.at("retained_indices").size() != requests.size()
                || requests.size() >= cohort_count)
                throw std::invalid_argument("preceding selection requires a partial decode batch within native capacity");

            for (const auto & index : selection.at("retained_indices"))
                pool_slots.push_back(static_cast<std::uint32_t>(input::integer(index, 0, cohort_count - 1)) + 1);
            if (!std::ranges::is_sorted(pool_slots) || std::ranges::adjacent_find(pool_slots) != pool_slots.end())
                throw std::invalid_argument("preceding selection must retain native member order");
        }
        const auto padding = cohort_count - requests.size();
        const bool preceding_prefill = phase == "prefill" && padding != 0;
        if (preceding_prefill && pool_slots.front() != padding + 1)
            throw std::invalid_argument("prefill reservation probes require a contiguous payload after the earlier running members");

        std::vector<std::uint32_t> generated;
        std::uint64_t prefill_prefix_tokens = 0;
        for (const auto & request : requests) {
            input::fields(request, { "input_tokens", "context_tokens", "prompt_tokens", "output_tokens", "finishes" });

            const auto prompt = measurement_count(request.at("prompt_tokens"));
            const auto context = measurement_count(request.at("context_tokens"));
            const auto input = measurement_count(request.at("input_tokens"));
            const auto output = input::positive(request.at("output_tokens"));
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

            const auto progress = phase == "prefill" ? 0 : context - prompt + 1;
            if (output <= progress || request.at("finishes").get<bool>() != (output == progress + 1))
                throw std::invalid_argument("measurement completion must agree with the declared output horizon");
            generated.push_back(progress);
        }

        const auto maximum_generated = *std::max_element(generated.begin(), generated.end());
        if (!pool_slots.empty() && std::ranges::any_of(generated, [maximum_generated](auto value) { return value != maximum_generated; }))
            throw std::invalid_argument("pool slot probes require simultaneously submitted payload requests");

        Json recipe = Json::array();
        std::uint32_t maximum_output = 0;
        for (std::size_t slot = 0; slot < requests.size(); ++slot) {
            const auto prompt = requests[slot].at("prompt_tokens").get<std::uint32_t>();
            const auto & representative = *representatives.lower_bound(prompt)->second;
            const auto output = input::positive(requests[slot].at("output_tokens"));
            maximum_output = std::max(maximum_output, output);
            // Use a real token prefix from the smallest sufficient source. The
            // request workload supplies content, not a serving arrival sequence.
            // The client gate proposes an admission opportunity while native
            // serving can already advance another Decode. Actual work qualifies
            // the requested progress; the release count is not a batch counter.
            const auto release = preceding_prefill ? 1 : maximum_generated - generated[slot];
            recipe.push_back(make_request(representative, prompt, output, release));
        }

        if (domain.contains("preceding_merge")) {
            const auto & merge = domain.at("preceding_merge");
            input::fields(merge, { "left_count", "right_count" });
            const auto left = input::positive(merge.at("left_count"));
            const auto right = input::positive(merge.at("right_count"));
            if (!preceding_filter || left + static_cast<std::uint64_t>(right) != cohort_count || right != requests.size() || pool_slots.front() != left + 1
                || maximum_generated != 1)
                throw std::invalid_argument("merge selection sampling requires the first Decode of the retained incoming prefill batch");

            std::uint64_t reserved_tokens = static_cast<std::uint64_t>(left) * 4 + (configuration.max_running_requests - left) * 3;
            std::uint64_t prefill_tokens = 0;
            for (const auto & request : recipe) {
                prefill_tokens += request.at("input_tokens").size();
                reserved_tokens += request.at("input_tokens").size() + request.at("output_tokens").get<std::uint32_t>();
            }
            if (configuration.max_running_requests > configuration.max_prefill_tokens || prefill_tokens > configuration.max_prefill_tokens
                || reserved_tokens > configuration.max_total_tokens)
                throw std::invalid_argument("merge selection prelude and payload exceed the declared token capacity");

            // Fill capacity, then retire blockers one Decode before the left
            // members. Queued payloads enter while those finished members are
            // still in running_batch; native serving merges, then filters them.
            Json cohort = Json::array();
            const auto & source = *representatives.begin()->second;
            for (std::size_t slot = 0; slot < configuration.max_running_requests; ++slot) cohort.push_back(make_request(source, 1, slot < left ? 3 : 2, 0));
            for (auto & request : recipe) cohort.push_back(std::move(request));
            recipe = std::move(cohort);
        }
        else if (!pool_slots.empty()) {
            // Decode selection removes fillers at the requested progress.
            // Prefill reserves earlier slots before releasing the payload;
            // those real requests remain live for the payload's output horizon.
            std::uint32_t filler_output = 1;
            if (preceding_filter) filler_output = maximum_generated;
            else if (preceding_prefill) {
                if (maximum_output == std::numeric_limits<std::uint32_t>::max())
                    throw std::invalid_argument("prefill reservation probe output horizon exceeds the native integer domain");
                filler_output = maximum_output + 1;
            }
            std::uint64_t prefill_tokens = padding;
            std::uint64_t reserved_tokens = (static_cast<std::uint64_t>(filler_output) + 1) * padding;
            for (const auto & request : recipe) {
                const auto prompt = request.at("input_tokens").size();
                prefill_tokens += prompt;
                reserved_tokens += prompt + request.at("output_tokens").get<std::uint32_t>();
            }
            if ((phase == "decode" && prefill_tokens > configuration.max_prefill_tokens) || reserved_tokens > configuration.max_total_tokens)
                throw std::invalid_argument("pool slot probes and payload exceed the declared token capacity");

            if (padding != 0) {
                const auto source = representatives.lower_bound(1);
                const auto & representative = *source->second;
                Json cohort = Json::array();
                std::size_t payload = 0;
                // The native framework owns these real requests and releases
                // their reservations at completion.
                for (std::size_t slot = 1; slot <= cohort_count; ++slot) {
                    if (payload < pool_slots.size() && slot == pool_slots[payload]) {
                        cohort.push_back(std::move(recipe[payload]));
                        ++payload;
                    }
                    else cohort.push_back(make_request(representative, 1, filler_output, 0));
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
