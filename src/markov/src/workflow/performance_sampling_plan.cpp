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
#include <fstream>
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

std::string measurement_kind(const Json & object) {
    const auto & kind = object.at("kind");
    if (kind == "cpu") return "cpu";
    if (kind == "collective" && object.at("features").at("execution") == "host_synchronous") return "cpu";
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

Json cpu_service_scope(const Json & features) {
    return {
        { "service", features.at("service") },
        {    "step",    features.at("step") },
        {   "phase",   features.at("phase") },
        {    "rank",    features.at("rank") }
    };
}

struct SelectedRequirement {
    Json row;
    const Json & combination; // Borrowed from this run's immutable prediction.
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
    MeasurementHistory(const Project & project, const Json & prediction, const Json & attempts) : attempts_(attempts) {
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
                const auto observed = input::read_json(input::resolve_path(path, manifest.at("environment")));
                item.environment = Environment::read(prepared.at("environment"));
                item.environment.measurement = observed.at("runtime").at("measurement_environment");
                item.prepared = {
                    {           "framework",           prepared.at("framework") },
                    { "model_configuration", prepared.at("model_configuration") },
                    {       "configuration",       prepared.at("configuration") }
                };
                item.kind = manifest.at("measurement_kind").get<std::string>();
                if (item.kind == "native_cpu_activity") {
                    const auto ranks = prepared.at("source_ranks").get<std::set<std::uint32_t>>();
                    for (const auto & features : prepared.at("cpu_services"))
                        if (ranks.contains(features.at("rank").get<std::uint32_t>())) item.cpu_services.insert(cpu_service_scope(features));
                }
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
                  const SamplingGroup & group, const Json & trigger, const Json * cpu_service = nullptr) const {
        auto evidence = Json::array();
        const auto target_limits = binding.control_limits(candidate.configuration);
        const auto trigger_key = trigger.dump();
        for (const auto & measurement : measurements_) {
            const bool applicable = measurement.declared || std::ranges::any_of(group.model_paths, [&](const Json & path) {
                                        return measurement.models.contains(path.get<std::string>());
                                    });
            if (!applicable || !matching_kind(group.kind, measurement.kind) || !measurement.cohorts.contains(trigger_key)) continue;
            if (cpu_service && !measurement.cpu_services.contains(*cpu_service)) continue;

            const auto & prepared = measurement.prepared;
            if (prepared.at("framework") != collection.name() || project.model_configuration.is_null()
                || prepared.at("model_configuration") != project.model_configuration)
                continue;
            if (!measurement.environment.shares_implementation(candidate.environment)
                || !measurement.environment.shares_measurement(candidate.environment, MeasurementScope::HostDevice))
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

        for (const auto & [key, attempt] : attempts_.items()) {
            const auto & status = attempt.at("status");
            if (status != "captured" && status != "no_candidates" && status != "failed") continue;

            const auto plan = Json::parse(key);
            if (plan.at("configuration_id") != candidate.id || plan.at("measurement_kind") != group.kind) continue;
            if (cpu_service) {
                const auto & services = plan.at("cpu_services");
                if (std::ranges::find(services, *cpu_service) == services.end()) continue;
            }
            for (const auto & previous : plan.at("native_work")) {
                if (previous != trigger) continue;
                Json record{
                    { "sampling_key",                     key },
                    {       "status",                  status },
                    {    "manifests", attempt.at("manifests") },
                    {         "work",                 trigger }
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
            { "source_manifests",                                                                                       std::move(sources) },
            {      "unavailable",                                                                                             unavailable_ },
            {         "boundary", "native cohort history under the same observed environment and control limits; not object cost coverage" }
        };
    }

private:
    struct Measurement {
        std::string manifest_path;
        std::set<std::string> models;
        bool declared = false;
        std::string kind;
        Json prepared;
        Environment environment;
        std::set<std::string> cohorts;
        std::set<Json> cpu_services;
    };

    static bool matching_kind(std::string_view requested, std::string_view observed) {
        if (requested == "cpu") return observed == "native_cpu_activity";
        if (requested == "transfer") return observed == "native_serving_timing" || observed == "native_cpu_activity";
        return observed == "native_device_work";
    }

    std::vector<Measurement> measurements_;
    const Json & attempts_; // Borrowed from the caller's durable ledger during run().
    Json unavailable_ = Json::array();
};

} // namespace

nlohmann::json PerformanceSamplingPlan::run(const Project & project, const Json & prediction, const Json & attempts) const {
    std::map<std::string, std::size_t> contexts;
    const auto & representative_groups = prediction.at("representative_collection").at("groups");
    for (std::size_t index = 0; index < representative_groups.size(); ++index)
        for (const auto & configuration : representative_groups[index].at("configuration_ids")) contexts.emplace(configuration.get<std::string>(), index);

    std::map<std::string, SelectedRequirement> selected;
    auto pending = Json::array();
    for (const auto & combination : prediction.at("combinations")) {
        const auto requirements = combination.find("performance_requirements");
        if (requirements == combination.end() || !requirements->contains("data_file")) continue;
        const auto configuration = combination.at("configuration_id").get<std::string>();
        const auto workload = combination.at("request_workload_id").get<std::string>();
        std::ifstream stream;
        stream.exceptions(std::ios::badbit | std::ios::failbit);
        stream.open(requirements->at("data_file").get<std::string>(), std::ios::binary);
        const auto data = Json::from_cbor(stream);
        for (const auto & row : data.at("objects")) {
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
                { "context", contexts.at(configuration) },
                {  "object",           row.at("object") }
            }.dump();
            auto [entry, inserted] = selected.try_emplace(key, row, combination);
            auto & requirement = entry->second;
            if (inserted) {
                requirement.row["work_origin"] = data.at("origins").at(row.at("work_origin").get<std::size_t>());
                requirement.row["consumers"] = Json::array();
            }
            requirement.row["consumers"].push_back(std::move(consumer));
        }
    }

    std::map<std::string, SamplingGroup> grouped;
    for (auto & [identity, requirement] : selected) {
        const auto & combination = requirement.combination;
        const auto kind = measurement_kind(requirement.row.at("object"));
        const auto key = Json{
            { "configuration_id",    combination.at("configuration_id") },
            {      "workload_id", combination.at("request_workload_id") },
            { "measurement_kind",                                  kind }
        }.dump();
        auto [entry, inserted] = grouped.try_emplace(key);
        auto & group = entry->second;
        if (inserted) {
            group.configuration_id = combination.at("configuration_id").get<std::string>();
            group.workload_id = combination.at("request_workload_id").get<std::string>();
            group.kind = kind;
            group.model_paths = combination.at("cost_models");
            // A new candidate can cover other feature points in this shared
            // execution context, including combinations not yet expanded.
            for (const auto & configuration : representative_groups.at(contexts.at(group.configuration_id)).at("configuration_ids"))
                group.configuration_ids.insert(configuration.get<std::string>());
        }
        group.requirements.push_back(std::move(requirement.row));
    }

    const MeasurementHistory history(project, prediction, attempts);
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

            std::set<std::size_t> used;
            std::set<Json> cpu_services;
            for (auto & assignment : assignments) {
                const auto & object = group.requirements.at(assignment.at("requirement_index").get<std::size_t>()).at("object");
                const auto cpu_service = object.at("kind") == "cpu" ? std::optional(cpu_service_scope(object.at("features"))) : std::nullopt;
                auto fresh = Json::array();
                auto previous_attempts = Json::array();
                for (const auto & point_index : assignment.at("point_indices")) {
                    const auto index = point_index.get<std::size_t>();
                    const auto previous = history.previous(project,
                                                           candidate,
                                                           **collection,
                                                           **binding,
                                                           group,
                                                           measurement_trigger(grid.at(index).at("requests")),
                                                           cpu_service ? &*cpu_service : nullptr);
                    if (!previous.empty()) {
                        for (const auto & attempt : previous) previous_attempts.push_back(attempt);
                        continue;
                    }
                    fresh.push_back(index);
                    used.insert(index);
                }
                assignment["point_indices"] = std::move(fresh);
                if (cpu_service && !assignment.at("point_indices").empty()) cpu_services.insert(*cpu_service);
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
                    const auto trigger = measurement_trigger(grid.at(index).at("requests"));
                    const auto attempted = [&](const Json * service) {
                        return !history.previous(project, candidate, **collection, **binding, group, trigger, service).empty();
                    };
                    const bool previous =
                        group.kind == "cpu" ? std::ranges::all_of(cpu_services, [&](const auto & service) { return attempted(&service); }) : attempted(nullptr);
                    if (previous) continue;
                    lifecycle_points.push_back(index);
                    used.insert(index);
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
            // A cohort does not prove that every CPU role or named interval
            // was requested. This is attempt scope, never measured coverage.
            if (group.kind == "cpu") key["cpu_services"] = cpu_services;
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
            groups.push_back(std::move(planned_group));
            break;
        }
    }
    return {
        {  "status",                                                                                              "planned" },
        {  "groups",                                                                                      std::move(groups) },
        { "pending",                                                                                     std::move(pending) },
        { "history",                                                                                     history.describe() },
        {   "scope", "actual target work including named CPU intervals; unexpanded target feature points remain unresolved" }
    };
}

} // namespace markov
