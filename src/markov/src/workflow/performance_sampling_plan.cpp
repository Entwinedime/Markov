#include "markov/workflow/performance_sampling_plan.hpp"

#include "markov/adapters/collection_adapter.hpp"
#include "markov/adapters/template_adapter.hpp"
#include "markov/contracts/json_input.hpp"
#include "markov/workflow/project.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
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
    if (kind == "cpu") return "cpu";
    if (kind == "transfer" && object.at("features").at("execution") == "synchronous_host_driver") return "transfer";
    return "device";
}

Json measurement_trigger(const Json & requests) {
    auto result = Json::array();
    for (const auto & request : requests)
        result.push_back({
            {        "prompt_tokens",  request.at("input_tokens").size() },
            {        "output_tokens",        request.at("output_tokens") },
            { "release_after_tokens", request.at("release_after_tokens") }
        });
    return result;
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

// History prevents repeated native work. Cost applicability remains owned by
// CostQuery/NodeCosts; observing a model batch does not reproduce its prior state.
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
                const auto prepared = input::read_json(input::resolve_path(path, manifest.at("input")));
                item.prepared = {
                    {     "framework",     prepared.at("framework") },
                    {    "model_path",    prepared.at("model_path") },
                    { "configuration", prepared.at("configuration") },
                    {   "environment",   prepared.at("environment") }
                };
                item.kind = manifest.at("measurement_kind").get<std::string>();
                for (const auto & point : prepared.at("measurement_grid")) item.cohorts.insert(measurement_trigger(point.at("requests")).dump());
                measurements_.push_back(std::move(item));
            }
            catch (const std::exception & error) {
                // A self-contained cost candidate may remain usable when its
                // optional provenance is absent. No native history is invented.
                unavailable_.push_back({
                    { "source_manifest",                                           path },
                    {          "reason", "independent_measurement_metadata_unavailable" },
                    {          "detail",                                   error.what() }
                });
            }
        }
    }

    Json previous(const Project & project, const Candidate & candidate, const CollectionAdapter & collection, const TemplateAdapter & binding,
                  const SamplingGroup & group, const Json & trigger) const {
        auto evidence = Json::array();
        const auto target_limits = binding.control_limits(candidate.configuration);
        const auto trigger_key = trigger.dump();
        for (const auto & measurement : measurements_) {
            const bool applicable = measurement.declared || std::ranges::any_of(group.model_paths, [&](const Json & path) {
                                        return measurement.models.contains(path.get<std::string>());
                                    });
            if (!applicable || !matching_kind(group.kind, measurement.kind) || !measurement.cohorts.contains(trigger_key)) continue;

            const auto & prepared = measurement.prepared;
            if (prepared.at("framework") != collection.name() || prepared.at("model_path") != project.model_path.string()) continue;
            const auto & deployment = prepared.at("environment");
            if (deployment.at("service") != candidate.environment.service || deployment.at("devices") != candidate.environment.devices
                || deployment.at("runtime") != "docker")
                continue;
            const auto source_configuration = binding.source_configuration(prepared.at("configuration"));
            if (!binding.shares_execution_path(source_configuration, candidate.configuration)) continue;
            const auto source_limits = binding.control_limits(source_configuration);
            // The same arrivals can produce different batches under a different
            // admission budget. Cost sharing does not imply sampling equivalence.
            if (!source_limits || !target_limits || source_limits->request_capacity != target_limits->request_capacity
                || source_limits->kv_capacity_tokens != target_limits->kv_capacity_tokens
                || source_limits->prefill_capacity_tokens != target_limits->prefill_capacity_tokens)
                continue;

            evidence.push_back({
                { "source_manifest",            measurement.manifest_path },
                {        "boundary", "previous_native_measurement_cohort" },
                {            "work",                              trigger }
            });
            return evidence;
        }

        if (!attempts_) return evidence;
        for (const auto & [key, attempt] : attempts_->items()) {
            const auto & status = attempt.at("status");
            if (status != "captured" && status != "no_candidates" && status != "failed") continue;

            const auto plan = Json::parse(key);
            if (plan.at("configuration_id") != candidate.id || plan.at("measurement_kind") != group.kind) continue;
            for (const auto & previous : plan.at("native_work")) {
                if (previous != trigger) continue;
                Json record{
                    { "sampling_key",                    key },
                    {       "status",                 status },
                    {     "manifest", attempt.at("manifest") },
                    {         "work",                trigger }
                };
                const auto reason = attempt.find("reason");
                if (reason != attempt.end()) record["reason"] = *reason;
                evidence.push_back(std::move(record));
                return evidence;
            }
        }
        return evidence;
    }

    Json describe() const {
        auto sources = Json::array();
        for (const auto & measurement : measurements_) sources.push_back(measurement.manifest_path);
        return {
            { "source_manifests",                                                                             std::move(sources) },
            {      "unavailable",                                                                                   unavailable_ },
            {         "boundary", "native cohort history under the same deployment and control limits; not object cost coverage" }
        };
    }

private:
    struct Measurement {
        std::string manifest_path;
        std::set<std::string> models;
        bool declared = false;
        std::string kind;
        Json prepared;
        std::set<std::string> cohorts;
    };

    static bool matching_kind(std::string_view requested, std::string_view observed) {
        if (requested == "cpu") return observed == "native_cpu_activity";
        if (requested == "transfer") return observed == "native_serving_timing" || observed == "native_cpu_activity";
        return observed == "native_device_work";
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
            if (row.at("coverage") != "needs_data") {
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
            // A new candidate can cover other feature points in this shared
            // execution context, including combinations not yet expanded.
            for (const auto & configuration : representative_groups.at(contexts.at(group.configuration_id)).at("configuration_ids"))
                group.configuration_ids.insert(configuration.get<std::string>());
        }
        group.requirements.push_back(std::move(requirement.row));
    }

    // Read each applicable catalog once, and only when CPU sampling is needed.
    // Definitions supply interval identities; target queries still own point coverage.
    std::set<std::string> cpu_configurations;
    for (const auto & [key, group] : grouped)
        if (group.kind == "cpu") cpu_configurations.insert(group.configuration_id);

    std::map<std::string, std::set<std::string>> catalogs;
    for (const auto & combination : prediction.at("combinations")) {
        const auto configuration = combination.at("configuration_id").get<std::string>();
        if (!cpu_configurations.contains(configuration)) continue;
        const auto requirements = combination.find("performance_requirements");
        if (requirements != combination.end() && requirements->contains("cpu_service_catalog"))
            catalogs[requirements->at("cpu_service_catalog").get<std::string>()].insert(configuration);
    }
    std::map<std::string, std::map<std::string, Json>> cpu_services;
    for (const auto & [path, configurations] : catalogs) {
        const auto catalog = input::read_json(path);
        for (const auto & service : catalog.at("services")) {
            const auto key = service.dump();
            for (const auto & configuration : configurations) cpu_services[configuration].emplace(key, service);
        }
    }

    const MeasurementHistory history(project, prediction);
    auto groups = Json::array();
    for (const auto & [identity, group] : grouped) {
        const auto & target = project.configuration(group.configuration_id);
        const auto collection =
            std::ranges::find_if(collection_adapters_, [&](const CollectionAdapter * item) { return item->framework() == target.framework; });
        const auto binding = std::ranges::find_if(template_adapters_, [&](const TemplateAdapter * item) { return item->framework() == target.framework; });
        const auto & prompts = project.workload(group.workload_id);

        // An already requested admission budget may expose a different native
        // batching path. Keep storage and request-pool capacities unchanged;
        // try another configuration only after the earlier recipe is exhausted.
        std::vector<const Candidate *> candidates{ &target };
        const auto limits = (*binding)->control_limits(target.configuration);
        if (limits) {
            for (const auto & candidate : project.configurations) {
                if (candidate.id == target.id || !group.configuration_ids.contains(candidate.id)) continue;
                if (candidate.environment.service != target.environment.service || candidate.environment.devices != target.environment.devices) continue;
                if (!(*binding)->shares_execution_path(target.configuration, candidate.configuration)) continue;
                const auto alternative = (*binding)->control_limits(candidate.configuration);
                if (alternative && alternative->request_capacity == limits->request_capacity && alternative->kv_capacity_tokens == limits->kv_capacity_tokens
                    && alternative->prefill_capacity_tokens != limits->prefill_capacity_tokens)
                    candidates.push_back(&candidate);
            }
        }

        for (std::size_t observation = 0; observation < candidates.size(); ++observation) {
            const auto & candidate = *candidates[observation];
            auto recipe = (*collection)->sampling_requests(candidate, prompts.requests, group.requirements);
            auto assignments = std::move(recipe["assignments"]);
            auto requests = std::move(recipe["measurement_requests"]);
            auto grid = std::move(recipe["measurement_grid"]);
            auto earlier = Json::array();
            for (const auto & point : grid)
                earlier.push_back(history.previous(project, candidate, **collection, **binding, group, measurement_trigger(point.at("requests"))));

            std::set<std::size_t> used;
            for (auto & assignment : assignments) {
                auto fresh = Json::array();
                auto previous_attempts = Json::array();
                for (const auto & point_index : assignment.at("point_indices")) {
                    const auto index = point_index.get<std::size_t>();
                    const auto & previous = earlier.at(index);
                    if (!previous.empty()) {
                        for (const auto & attempt : previous) previous_attempts.push_back(attempt);
                        continue;
                    }
                    fresh.push_back(index);
                    used.insert(index);
                }
                assignment["point_indices"] = std::move(fresh);
                if (assignment.at("point_indices").empty()) {
                    assignment["status"] = "previous_sampling_attempt_without_new_trigger";
                    assignment["reason"] = "previous_sampling_attempt_without_new_trigger";
                }
                if (!previous_attempts.empty()) assignment["previous_sampling_attempts"] = std::move(previous_attempts);
            }
            for (const auto & unresolved : recipe.at("unresolved")) {
                auto assignment = unresolved;
                assignment["point_indices"] = Json::array();
                assignment["status"] = "unresolved";
                assignments.push_back(std::move(assignment));
            }

            if (used.empty() && observation + 1 < candidates.size()) continue;

            Json lifecycle_points = Json::array();
            if (!used.empty()) {
                for (const auto & point : recipe.at("lifecycle_points")) {
                    const auto index = point.get<std::size_t>();
                    if (!earlier.at(index).empty()) continue;
                    lifecycle_points.push_back(index);
                    used.insert(index);
                }
            }

            std::optional<std::size_t> empty_schedule_point;
            if (group.kind == "cpu" && !used.empty()
                && std::ranges::any_of(cpu_services.at(group.configuration_id), [](const auto & entry) { return entry.second.at("phase") == "idle"; })) {
                // Cover the known empty-control mechanism in the same CPU run,
                // before later timing exposes it as a target cost requirement.
                const Json idle{
                    {    "phase",        "idle" },
                    { "requests", Json::array() }
                };
                const auto found = std::ranges::find(requests, idle);
                const auto index = found == requests.end() ? requests.size() : static_cast<std::size_t>(found - requests.begin());
                if (found == requests.end()) {
                    auto probe = (*collection)->workloads(candidate, prompts.requests, Json::array({ idle }));
                    probe.at(0)["id"] = index;
                    requests.push_back(idle);
                    grid.push_back(std::move(probe.at(0)));
                    earlier.push_back(history.previous(project, candidate, **collection, **binding, group, measurement_trigger(grid.at(index).at("requests"))));
                }
                if (earlier.at(index).empty()) {
                    used.insert(index);
                    empty_schedule_point = index;
                }
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
                { "configuration_id",  candidate.id },
                { "measurement_kind",    group.kind },
                {      "native_work", Json::array() }
            };
            for (const auto index : used) key["native_work"].push_back(measurement_trigger(grid.at(index).at("requests")));
            Json cpu_points = Json::array();
            if (group.kind == "cpu") {
                for (const auto & requirement : group.requirements) cpu_points.push_back(requirement.at("object").at("features"));
                // An idle recipe triggers a mechanism, not a zero-member work
                // label: filtering may still inspect an existing native batch.
                // Static descriptors let sampling record its actual work.
            }
            Json planned_group{
                {                  "key",                                                                      key.dump() },
                {     "configuration_id",                                                                    candidate.id },
                {    "configuration_ids",                                                         group.configuration_ids },
                {          "workload_id",                                                               group.workload_id },
                {     "measurement_kind",                                                                      group.kind },
                { "measurement_requests",                                                             std::move(retained) },
                {       "lifecycle_gaps",                                                     recipe.at("lifecycle_gaps") },
                {         "requirements",                                                              group.requirements },
                {          "assignments",                                                          std::move(assignments) },
                {               "status",                                                  ready ? "ready" : "needs_data" },
                {               "reason", ready ? Json(nullptr) : Json("no_new_reachable_independent_measurement_domain") }
            };
            if (!lifecycle_points.empty()) {
                planned_group["mechanism_points"]["request_lifecycles"] = Json::array();
                for (const auto & index : lifecycle_points)
                    planned_group["mechanism_points"]["request_lifecycles"].push_back(relocated.at(index.get<std::size_t>()));
            }
            if (group.kind == "cpu") {
                planned_group["cpu_services"] = std::move(cpu_points);
                planned_group["cpu_service_descriptors"] = Json::array();
                for (const auto & [key, service] : cpu_services[group.configuration_id]) planned_group["cpu_service_descriptors"].push_back(service);
                if (empty_schedule_point) planned_group["mechanism_points"]["empty_schedule"] = relocated.at(*empty_schedule_point);
            }
            groups.push_back(std::move(planned_group));
            break;
        }
    }
    return {
        {  "status",                                                                                                       "planned" },
        {  "groups",                                                                                               std::move(groups) },
        { "pending",                                                                                              std::move(pending) },
        { "history",                                                                                              history.describe() },
        {   "scope", "actual target points plus source CPU interval descriptors; unexpanded target feature points remain unresolved" }
    };
}

} // namespace markov
