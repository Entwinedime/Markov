#include "markov/workflow/performance_sampling_plan.hpp"

#include "markov/adapters/collection_adapter.hpp"
#include "markov/adapters/template_adapter.hpp"
#include "markov/contracts/json_input.hpp"
#include "markov/workflow/project.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace markov {
namespace {

using Json = nlohmann::json;

Json object_identity(const Json & object) {
    return {
        {                 "kind",                 object.at("kind") },
        {                 "name",                 object.at("name") },
        { "measurement_boundary", object.at("measurement_boundary") },
        {             "features",             object.at("features") }
    };
}

std::string measurement_kind(const Json & object) {
    const auto & kind = object.at("kind");
    if (kind == "transfer" && object.at("features").at("execution") == "synchronous_host_driver") return "transfer";
    return "device";
}

std::set<std::string> kernel_operand_fields(const Json & requirement) {
    const auto & object = requirement.at("object");
    if (object.at("kind") != "kernel") return {};
    const auto operands = object.at("features").find("operand_work");
    if (operands == object.at("features").end()) return {};

    std::set<std::string> fields;
    for (const auto & [name, value] : operands->items()) fields.insert(name);
    return fields;
}

std::set<std::string> transfer_executions(const Json & requirement) {
    const auto & object = requirement.at("object");
    if (object.at("kind") == "transfer" && object.at("features").at("execution") == "synchronous_host_driver") return { "synchronous_host_driver" };
    return {};
}

struct ObservationRequirement {
    std::string_view plan_key;
    std::string_view summary_key;
    std::string_view field;
    std::set<std::string> required;
};

std::array<ObservationRequirement, 2> observation_requirements(const Json & requirement) {
    return {
        { { "kernel_operand_fields", "observed_kernel_operand_fields", "fields", kernel_operand_fields(requirement) },
         { "transfer_executions", "observed_transfer_executions", "executions", transfer_executions(requirement) } }
    };
}

struct SelectedRequirement {
    Json row;
    std::string configuration_id;
    std::string workload_id;
    Json model_paths;
};

struct SamplingGroup {
    std::string configuration_id;
    std::string workload_id;
    std::string kind;
    Json requirements = Json::array();
    Json model_paths;
    std::set<std::string> configuration_ids;
};

// Only independent sampling metadata is read here. Candidate timing keys still
// belong to CostQuery/NodeCosts; a previously seen batch is never a cost match.
class MeasurementHistory {
public:
    MeasurementHistory(const Project & project, const Json & prediction) {
        const auto attempts = prediction.find("performance_sampling");
        if (attempts != prediction.end()) attempts_ = &*attempts;

        std::map<std::string, Measurement> declarations;
        for (const auto & path : project.performance_manifests) {
            const auto source = path.lexically_normal().string();
            auto & item = declarations[source];
            item.manifest_path = source;
            item.declared = true;
        }
        for (const auto & model : prediction.at("cost_models")) {
            if (model.at("status") != "loaded") continue;
            const auto source = std::filesystem::path(model.at("source_manifest").get<std::string>()).lexically_normal().string();
            auto & item = declarations[source];
            item.manifest_path = source;
            item.models.insert(model.at("path").get<std::string>());
        }

        // A successful capture can have no usable cost candidates. Its observed
        // domains still prevent another capture of an unchanged missing point.
        for (auto & [path, item] : declarations) {
            try {
                const auto manifest = input::read_json(path);
                if (manifest.at("role") != "performance") {
                    unavailable_.push_back({
                        { "source_manifest",                                        path },
                        {          "reason", "independent_measurement_metadata_required" }
                    });
                    continue;
                }
                const auto coverage = manifest.find("feature_coverage");
                if (coverage == manifest.end()) {
                    unavailable_.push_back({
                        { "source_manifest",                                              path },
                        {          "reason", "independent_measurement_domain_metadata_missing" }
                    });
                    continue;
                }
                const auto prepared = input::read_json(input::resolve_path(path, manifest.at("input")));
                item.prepared = {
                    {     "framework",     prepared.at("framework") },
                    {    "model_path",    prepared.at("model_path") },
                    { "configuration", prepared.at("configuration") },
                    {   "environment",   prepared.at("environment") }
                };
                item.kind = manifest.at("measurement_kind").get<std::string>();
                item.coverage = *coverage;
                measurements_.push_back(std::move(item));
            }
            catch (const std::exception & error) {
                // Missing historical metadata does not invalidate a self-contained
                // cost candidate. It leaves the no-repeat evidence unresolved.
                unavailable_.push_back({
                    { "source_manifest",                                           path },
                    {          "reason", "independent_measurement_metadata_unavailable" },
                    {          "detail",                                   error.what() }
                });
            }
        }
    }

    Json observed(const Project & project, const Candidate & candidate, const CollectionAdapter & collection, const TemplateAdapter & binding,
                  const SamplingGroup & group, const Json & requirement, const Json & domain) const {
        auto evidence = Json::array();
        const auto required = observation_requirements(requirement);
        const auto target_limits = binding.control_limits(candidate.configuration);
        for (const auto & measurement : measurements_) {
            const bool applicable = measurement.declared || std::ranges::any_of(group.model_paths, [&](const Json & path) {
                                        return measurement.models.contains(path.get<std::string>());
                                    });
            if (!applicable || !matching_kind(group.kind, measurement.kind)) continue;

            const auto & prepared = measurement.prepared;
            if (prepared.at("framework") != collection.name() || prepared.at("model_path") != project.model_path.string()) continue;
            const auto & deployment = prepared.at("environment");
            if (deployment.at("service") != candidate.environment.service || deployment.at("devices") != candidate.environment.devices
                || deployment.at("runtime") != "docker")
                continue;
            const auto source_configuration = binding.source_configuration(prepared.at("configuration"));
            if (!binding.shares_execution_path(source_configuration, candidate.configuration)) continue;
            const auto source_limits = binding.control_limits(source_configuration);
            if (!source_limits || !target_limits || source_limits->request_capacity != target_limits->request_capacity
                || source_limits->kv_capacity_tokens != target_limits->kv_capacity_tokens)
                continue;

            // Filtering is post-publication work. Its physical selection must
            // be observed directly, not inferred from a later model batch.
            if (domain.contains("batch_filter")) {
                if (observed_filter_work(measurement.coverage, domain, candidate.environment.devices.size()))
                    evidence.push_back({
                        { "source_manifest",           measurement.manifest_path },
                        {        "boundary", "observed_native_batch_filter_work" }
                    });
                continue;
            }

            // Batch exposure alone cannot close a new observation contract.
            // Each required fact must have been recorded on every rank.
            bool instrumented = true;
            for (const auto & fact : required) {
                if (!fact.required.empty()
                    && !observed_instrumentation(measurement.coverage,
                                                 domain,
                                                 fact.summary_key,
                                                 fact.field,
                                                 fact.required,
                                                 candidate.environment.devices.size())) {
                    instrumented = false;
                    break;
                }
            }
            if (!instrumented) continue;

            std::string_view boundary;
            if (completed_domain(measurement.coverage, domain, candidate.environment.devices.size())) boundary = "completed_requested_work";
            else if (group.kind == "transfer" && requirement.at("work_origin").at("step") != "publish"
                     && prepublication_domain(measurement.coverage, domain, candidate.environment.devices.size()))
                boundary = "observed_prepublication_batch_work";
            if (!boundary.empty())
                evidence.push_back({
                    { "source_manifest", measurement.manifest_path },
                    {        "boundary",                  boundary }
                });
        }
        return evidence;
    }

    Json attempted(const SamplingGroup & group, const Json & requirement, const Json & domain) const {
        auto evidence = Json::array();
        if (!attempts_) return evidence;
        const auto required = observation_requirements(requirement);
        for (const auto & [key, attempt] : attempts_->items()) {
            const auto & status = attempt.at("status");
            if (status != "captured" && status != "no_candidates" && status != "failed") continue;

            // The planner owns this key's JSON meaning; Python persists it
            // opaquely. Compare individual points so regrouping cannot retry a
            // failed or unproductive point as a smaller request list.
            const auto plan = Json::parse(key);
            if (plan.at("configuration_id") != group.configuration_id || plan.at("measurement_kind") != group.kind) continue;

            bool requested_all = true;
            for (const auto & fact : required) {
                if (fact.required.empty()) continue;
                const auto requested = plan.find(fact.plan_key);
                if (requested == plan.end() || !std::ranges::includes(requested->get<std::set<std::string>>(), fact.required)) {
                    requested_all = false;
                    break;
                }
            }
            if (!requested_all) continue;

            for (const auto & previous : plan.at("measurement_requests")) {
                if (!same_domain(previous, domain)) continue;
                Json record{
                    {        "plan",        attempt.at("plan") },
                    { "group_index", attempt.at("group_index") },
                    {      "status",                    status },
                    {    "manifest",    attempt.at("manifest") },
                    {        "work",                    domain }
                };
                const auto reason = attempt.find("reason");
                if (reason != attempt.end()) record["reason"] = *reason;
                evidence.push_back(std::move(record));
                break;
            }
        }
        return evidence;
    }

    Json describe() const {
        auto sources = Json::array();
        for (const auto & measurement : measurements_) sources.push_back(measurement.manifest_path);
        return {
            { "source_manifests",                                                                           std::move(sources) },
            {      "unavailable",                                                                                 unavailable_ },
            {         "boundary", "independent request/batch metadata only; prior domain exposure is not object cost coverage" }
        };
    }

private:
    struct Measurement {
        std::string manifest_path;
        std::set<std::string> models;
        bool declared = false;
        std::string kind;
        Json prepared;
        Json coverage;
    };

    static bool matching_kind(std::string_view requested, std::string_view observed) {
        return requested == "transfer" ? observed == "native_serving_timing" : observed == "native_device_work";
    }

    struct RequestMember {
        std::array<std::uint64_t, 3> work;
        std::optional<std::uint64_t> pool_slot;

        auto operator<=>(const RequestMember &) const = default;
    };

    static std::vector<RequestMember> request_members(const Json & requests, std::size_t count, bool with_slots = false) {
        std::vector<RequestMember> members;
        for (std::size_t index = 0; index < count; ++index) {
            const auto & request = requests.at(index);
            std::optional<std::uint64_t> slot;
            if (with_slots) {
                const auto actual = request.find("request_pool_slot");
                if (actual != request.end() && !actual->is_null()) slot = actual->get<std::uint64_t>();
            }
            members.push_back({
                { request.at("input_tokens").get<std::uint64_t>(),
                 request.at("context_tokens").get<std::uint64_t>(),
                 request.at("prompt_tokens").get<std::uint64_t>() },
                slot
            });
        }
        std::ranges::sort(members);
        return members;
    }

    static std::vector<RequestMember> requested_members(const Json & domain, std::size_t count) {
        auto requests = domain.at("requests");
        const auto slots = domain.find("pool_slots");
        if (slots != domain.end())
            for (std::size_t index = 0; index < count; ++index) requests.at(index)["request_pool_slot"] = slots->at(index);

        // Pair work and coordinates before sorting. Separate multisets could
        // attribute one request's measured slot to another request's work.
        return request_members(requests, count, slots != domain.end());
    }

    static std::vector<RequestMember> requested_members(const Json & domain) { return requested_members(domain, domain.at("requests").size()); }

    static bool same_domain(const Json & left, const Json & right) {
        if (left.contains("batch_filter") || right.contains("batch_filter"))
            return left.contains("batch_filter") && right.contains("batch_filter") && left.at("batch_filter") == right.at("batch_filter");
        if (left.contains("pool_slots") != right.contains("pool_slots")) return false;

        // Recipe order only assigns completion to the first K members. Member
        // order itself is irrelevant, but the completed subset is a new trigger.
        return left.at("phase") == right.at("phase") && requested_members(left) == requested_members(right)
               && requested_members(left, left.at("finished_request_count").get<std::size_t>())
                      == requested_members(right, right.at("finished_request_count").get<std::size_t>());
    }

    static bool observed_filter_work(const Json & coverage, const Json & domain, std::size_t ranks) {
        const auto observed = coverage.find("observed_batch_filter_work");
        if (observed == coverage.end()) return false;

        std::set<std::size_t> complete;
        for (const auto & work : *observed) {
            if (work.at("batch_filter") != domain.at("batch_filter")) continue;
            if (!std::ranges::all_of(work.at("requests"), [&](const Json & request) {
                    return request.at("prompt_tokens") == domain.at("prompt_tokens") && request.at("generated_tokens") == 1;
                }))
                continue;

            const auto fields = work.at("fields").get<std::set<std::string>>();
            if (fields.contains("batch_filter")) complete.insert(work.at("rank").get<std::size_t>());
        }
        return complete.size() == ranks;
    }

    static bool observed_instrumentation(const Json & coverage, const Json & domain, std::string_view summary, std::string_view field,
                                         const std::set<std::string> & required, std::size_t ranks) {
        const auto observed = coverage.find(summary);
        if (observed == coverage.end()) return false;

        const auto members = requested_members(domain);
        std::set<std::size_t> complete;
        for (const auto & batch : *observed) {
            if (batch.at("phase") != domain.at("phase")
                || request_members(batch.at("requests"), batch.at("requests").size(), domain.contains("pool_slots")) != members)
                continue;
            const auto available = batch.at(field).get<std::set<std::string>>();
            if (std::ranges::includes(available, required)) complete.insert(batch.at("rank").get<std::size_t>());
        }
        return complete.size() == ranks;
    }

    static bool completed_domain(const Json & coverage, const Json & domain, std::size_t ranks) {
        const auto members = requested_members(domain);
        const auto finished = requested_members(domain, domain.at("finished_request_count").get<std::size_t>());
        std::set<std::size_t> observed;
        for (const auto & batch : coverage.at("observed_completed_batch_features"))
            if (batch.at("phase") == domain.at("phase")
                && request_members(batch.at("requests"), batch.at("requests").size(), domain.contains("pool_slots")) == members
                && request_members(batch.at("finished_requests"), batch.at("finished_requests").size(), domain.contains("pool_slots")) == finished)
                observed.insert(batch.at("rank").get<std::size_t>());
        return observed.size() == ranks;
    }

    static bool prepublication_domain(const Json & coverage, const Json & domain, std::size_t ranks) {
        const auto members = requested_members(domain);
        std::set<std::size_t> observed;
        for (const auto & batch : coverage.at("observed_batch_features")) {
            if (batch.at("phase") != domain.at("phase")) continue;
            if (request_members(batch.at("requests"), batch.at("requests").size(), domain.contains("pool_slots")) != members) continue;
            observed.insert(batch.at("rank").get<std::size_t>());
        }
        return observed.size() == ranks;
    }

    std::vector<Measurement> measurements_;
    const Json * attempts_ = nullptr; // Borrowed from the immutable prediction during run().
    Json unavailable_ = Json::array();
};

} // namespace

PerformanceSamplingPlan::PerformanceSamplingPlan(std::span<const CollectionAdapter * const> collection_adapters,
                                                 std::span<const TemplateAdapter * const> template_adapters)
    : collection_adapters_(collection_adapters.begin(), collection_adapters.end()),
      template_adapters_(template_adapters.begin(), template_adapters.end()) {}

nlohmann::json PerformanceSamplingPlan::run(const Project & project, const Json & prediction) const {
    std::map<std::string, std::size_t> contexts;
    const auto & representative_groups = prediction.at("representative_collection").at("groups");
    for (std::size_t index = 0; index < representative_groups.size(); ++index)
        for (const auto & configuration : representative_groups[index].at("configuration_ids")) contexts.emplace(configuration.get<std::string>(), index);

    std::map<std::string, SelectedRequirement> selected;
    auto pending = Json::array();
    for (const auto & combination : prediction.at("combinations")) {
        const auto requirements = combination.find("performance_requirements");
        if (requirements == combination.end()) continue;
        const auto configuration = combination.at("configuration_id").get<std::string>();
        const auto workload = combination.at("request_workload_id").get<std::string>();
        for (const auto & row : requirements->at("objects")) {
            if (row.at("coverage") == "candidate_available") continue;
            Json consumer{
                {    "configuration_id", configuration },
                { "request_workload_id",      workload }
            };
            if (row.at("coverage") != "needs_data" || !row.at("measurement_required").get<bool>()) {
                pending.push_back({
                    {   "object",                                                                                 row.at("object") },
                    { "consumer",                                                                                         consumer },
                    {   "status", row.at("coverage") == "contract_incomplete" ? "contract_incomplete" : "measurement_not_selected" },
                    {   "reason",                                                                                 row.at("reason") }
                });
                continue;
            }

            const auto key = Json{
                { "context",        contexts.at(configuration) },
                {  "object", object_identity(row.at("object")) }
            }.dump();
            auto [entry, inserted] = selected.try_emplace(key);
            auto & requirement = entry->second;
            if (inserted) {
                requirement.row = row;
                requirement.row["work_origin"] = row.at("measurement_origin");
                requirement.row["example"] = row.at("measurement_example");
                requirement.row["consumers"] = Json::array();
                requirement.configuration_id = configuration;
                requirement.workload_id = workload;
                requirement.model_paths = combination.at("cost_models");
            }
            requirement.row["consumers"].push_back(std::move(consumer));
        }
    }

    std::map<std::string, SamplingGroup> grouped;
    for (auto & [identity, requirement] : selected) {
        const auto kind = measurement_kind(requirement.row.at("object"));
        const auto key = Json{
            { "configuration_id", requirement.configuration_id },
            {      "workload_id",      requirement.workload_id },
            { "measurement_kind",                         kind }
        }.dump();
        auto [entry, inserted] = grouped.try_emplace(key);
        auto & group = entry->second;
        if (inserted) {
            group.configuration_id = requirement.configuration_id;
            group.workload_id = requirement.workload_id;
            group.kind = kind;
            group.model_paths = std::move(requirement.model_paths);
        }
        for (const auto & consumer : requirement.row.at("consumers")) group.configuration_ids.insert(consumer.at("configuration_id").get<std::string>());
        group.requirements.push_back(std::move(requirement.row));
    }

    const MeasurementHistory history(project, prediction);
    auto groups = Json::array();
    for (const auto & [identity, group] : grouped) {
        const auto & candidate = project.configuration(group.configuration_id);
        const auto collection =
            std::ranges::find_if(collection_adapters_, [&](const CollectionAdapter * item) { return item->framework() == candidate.framework; });
        const auto binding = std::ranges::find_if(template_adapters_, [&](const TemplateAdapter * item) { return item->framework() == candidate.framework; });
        const auto & prompts = project.workload(group.workload_id);
        auto recipe = (*collection)->sampling_requests(candidate, prompts.requests, group.requirements);
        auto assignments = std::move(recipe["assignments"]);
        auto requests = std::move(recipe["measurement_requests"]);
        std::set<std::size_t> used;
        for (auto & assignment : assignments) {
            const auto requirement_index = assignment.at("requirement_index").get<std::size_t>();
            auto fresh = Json::array();
            auto evidence = Json::array();
            auto previous_attempts = Json::array();
            for (const auto & point_index : assignment.at("point_indices")) {
                const auto index = point_index.get<std::size_t>();
                const auto observed =
                    history.observed(project, candidate, **collection, **binding, group, group.requirements.at(requirement_index), requests.at(index));
                if (!observed.empty()) {
                    for (const auto & source : observed) evidence.push_back(source);
                    continue;
                }
                const auto attempted = history.attempted(group, group.requirements.at(requirement_index), requests.at(index));
                if (!attempted.empty()) {
                    for (const auto & attempt : attempted) previous_attempts.push_back(attempt);
                    continue;
                }
                fresh.push_back(index);
                used.insert(index);
            }
            assignment["point_indices"] = std::move(fresh);
            if (assignment.at("point_indices").empty()) {
                const auto reason =
                    previous_attempts.empty() ? "observed_domain_without_required_measurement" : "previous_sampling_attempt_without_new_trigger";
                assignment["status"] = reason;
                assignment["reason"] = reason;
            }
            if (!evidence.empty()) assignment["previous_measurements"] = std::move(evidence);
            if (!previous_attempts.empty()) assignment["previous_sampling_attempts"] = std::move(previous_attempts);
        }
        for (const auto & unresolved : recipe.at("unresolved")) {
            auto assignment = unresolved;
            assignment["point_indices"] = Json::array();
            assignment["status"] = "unresolved";
            assignments.push_back(std::move(assignment));
        }

        Json retained = Json::array();
        std::map<std::size_t, std::size_t> relocated;
        for (const auto index : used) {
            relocated.emplace(index, retained.size());
            retained.push_back(std::move(requests.at(index)));
        }
        for (auto & assignment : assignments)
            for (auto & point : assignment.at("point_indices")) point = relocated.at(point.get<std::size_t>());

        const bool ready = !retained.empty();
        Json key{
            {     "configuration_id", group.configuration_id },
            {     "measurement_kind",             group.kind },
            { "measurement_requests",               retained }
        };
        std::map<std::string_view, std::set<std::string>> required;
        for (const auto & requirement : group.requirements) {
            for (const auto & fact : observation_requirements(requirement)) {
                auto & values = required[fact.plan_key];
                values.insert(fact.required.begin(), fact.required.end());
            }
        }
        for (const auto & [plan_key, values] : required)
            if (!values.empty()) key[plan_key] = values;

        groups.push_back({
            {                  "key",                                                                      key.dump() },
            {     "configuration_id",                                                          group.configuration_id },
            {    "configuration_ids",                                                         group.configuration_ids },
            {          "workload_id",                                                               group.workload_id },
            {     "measurement_kind",                                                                      group.kind },
            { "measurement_requests",                                                             std::move(retained) },
            {         "requirements",                                                              group.requirements },
            {          "assignments",                                                          std::move(assignments) },
            {               "status",                                                  ready ? "ready" : "needs_data" },
            {               "reason", ready ? Json(nullptr) : Json("no_new_reachable_independent_measurement_domain") }
        });
    }
    return {
        {  "status",                                                                                    "planned" },
        {  "groups",                                                                            std::move(groups) },
        { "pending",                                                                           std::move(pending) },
        { "history",                                                                           history.describe() },
        {   "scope", "actual named target requirements; source prototypes and unexpanded paths remain unresolved" }
    };
}

} // namespace markov
