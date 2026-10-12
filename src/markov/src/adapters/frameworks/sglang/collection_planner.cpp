#include "markov/adapters/frameworks/sglang/collection_planner.hpp"

#include "markov/adapters/frameworks/sglang/bindings.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace markov {
namespace {

struct ConfigurationGroup {
    const Candidate * representative;
    std::vector<const Candidate *> members;
};

} // namespace

nlohmann::json SGLangCollectionPlanner::candidate_recipe(const SGLangConfiguration & configuration) const {
    if (configuration.max_running_requests < 2) {
        return {
            { "status",                         "not_ready" },
            { "reason", "single_request_recipe_not_defined" }
        };
    }

    // These are mechanism triggers, not target workloads or duration samples.
    // Resolve P from the actual worker; do not copy a backend default here.
    using Json = nlohmann::json;
    auto groups = Json::array();
    groups.push_back({
        {             "purpose",                                                                         "prefill_direct_completion" },
        { "prompt_page_offsets",                                                                             Json::array({ -2, -1 }) },
        {       "output_tokens",                                                                               Json::array({ 1, 1 }) },
        { "expected_mechanisms", Json::array({ "admission_members", "last_location_members", "direct_kv_release", "shared_output" }) }
    });
    groups.push_back({
        {             "purpose","decode_page_crossing_and_partial_completion"                                },
        { "prompt_page_offsets",                        Json::array({ -1, 0 }) },
        {       "output_tokens",                         Json::array({ 4, 2 }) },
        { "expected_mechanisms",
         Json::array({ "decode_new_pages",
         "decode_no_new_pages",
         "partial_filter",
         "single_member_decode",
         "empty_release_group",
         "single_member_release_group" })                                     }
    });
    groups.push_back({
        {             "purpose",                                                   "decode_joint_completion" },
        { "prompt_page_offsets",                                                     Json::array({ -2, -1 }) },
        {       "output_tokens",                                                       Json::array({ 2, 2 }) },
        { "expected_mechanisms", Json::array({ "multi_member_release_group", "completed_batch_transition" }) }
    });

    return {
        {                       "status",                "requires_asset_coverage"                                         },
        {             "page_size_source",                                                 "effective_server_args.page_size" },
        {     "minimum_page_size_tokens",                                                                                 4 },
        {       "minimum_prefill_budget",                          { { "page_size_multiple", 2 }, { "offset_tokens", -1 } } },
        { "minimum_total_token_capacity",                           { { "page_size_multiple", 4 }, { "offset_tokens", 0 } } },
        {                   "submission",            "submit_each_group_together_and_wait_for_completion_before_next_group" },
        {                       "groups",                                                                 std::move(groups) },
        {         "uncovered_mechanisms",
         Json::array({ "running_batch_merge",
         "periodic_nonfinal_output",
         "admission_budget_stop",
         "memory_pressure",
         "backend_variants_outside_qualified_domains" })                                                                   },
        {                   "completion", "all_expected_mechanisms_observed_on_every_rank_and_template_relations_qualified" },
        {      "missing_evidence_action",                                         "stop_and_report_without_automatic_retry" }
    };
}

nlohmann::json SGLangCollectionPlanner::configurations(std::span<const Candidate> candidates) const {
    std::vector<ConfigurationGroup> groups;
    auto excluded = nlohmann::json::array();
    for (const auto & candidate : candidates) {
        std::string_view reason;
        if (candidate.framework != Framework::SGLang) reason = "framework_collection_planning_not_implemented";
        else {
            const auto & configuration = std::get<SGLangConfiguration>(candidate.configuration);
            const auto control = SGLangBindings{}.control_limits(configuration);
            if (!control) reason = control.error();
            else if (candidate.environment.devices.size() != configuration.tp_size) reason = "deployment_rank_count_mismatch";
        }
        if (!reason.empty()) {
            excluded.push_back({
                { "configuration_id", candidate.id },
                {           "reason",       reason }
            });
            continue;
        }

        const auto & configuration = std::get<SGLangConfiguration>(candidate.configuration);
        ConfigurationGroup * selected = nullptr;
        for (auto & group : groups) {
            const auto & source = *group.representative;
            if (source.environment.service != candidate.environment.service || source.environment.devices != candidate.environment.devices) continue;
            if (!SGLangBindings{}.supports_configuration_change(std::get<SGLangConfiguration>(source.configuration), configuration)) continue;
            selected = &group;
            break;
        }
        if (!selected) {
            groups.push_back({ &candidate, { &candidate } });
            continue;
        }

        selected->members.push_back(&candidate);
        // The current pool transform can shrink a captured table but cannot grow
        // one. Equal capacities retain declaration order; no metric is optimized.
        const auto & source_configuration = std::get<SGLangConfiguration>(selected->representative->configuration);
        if (configuration.max_running_requests > source_configuration.max_running_requests) selected->representative = &candidate;
    }

    auto result = nlohmann::json::array();
    for (const auto & group : groups) {
        auto members = nlohmann::json::array();
        for (const auto * candidate : group.members) members.push_back(candidate->id);
        result.push_back({
            { "representative_configuration_id",                                                                group.representative->id },
            {               "configuration_ids",                                                                                 members },
            {           "request_pool_capacity", std::get<SGLangConfiguration>(group.representative->configuration).max_running_requests },
            {                "candidate_recipe",    candidate_recipe(std::get<SGLangConfiguration>(group.representative->configuration)) }
        });
    }
    return {
        {                  "status",                                             groups.empty() ? "unsupported" : "requires_mechanism_coverage" },
        {                  "groups",                                                                                                     result },
        { "excluded_configurations",                                                                                                   excluded },
        {         "selection_basis", "same deployment and implemented configuration relation; largest request pool; declaration order for ties" }
    };
}

} // namespace markov
