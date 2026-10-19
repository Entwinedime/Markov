#include "markov/adapters/frameworks/sglang/template_adapter.hpp"

#include "markov/adapters/frameworks/sglang/bindings.hpp"
#include "markov/adapters/frameworks/sglang/template_bindings.hpp"
#include "markov/adapters/runtimes/ascend/source_extractor.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
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

constexpr std::array<std::string_view, 4> prefill_mechanisms{ "admission_members", "last_location_members", "direct_kv_release", "shared_output" };
constexpr std::array<std::string_view, 6> partial_completion_mechanisms{ "decode_new_pages",     "decode_no_new_pages", "partial_filter",
                                                                         "single_member_decode", "empty_release_group", "single_member_release_group" };
constexpr std::array<std::string_view, 2> joint_completion_mechanisms{ "multi_member_release_group", "completed_batch_transition" };
constexpr std::array<std::string_view, 2> queued_admission_mechanisms{ "single_member_prefill", "running_batch_merge" };

} // namespace

TemplateProduction SGLangTemplateAdapter::produce(const std::filesystem::path & manifest_path, nlohmann::json manifest,
                                                  const std::filesystem::path & work_directory) const {
    // Framework bindings outlive runtime extraction and its borrowed block declarations.
    const SGLangBindings bindings;
    AscendSourceExtractor extractor(bindings);
    TemplateProduction result{ extractor.extract(manifest_path, std::move(manifest), work_directory), std::nullopt };
    if (result.manifest.at("graph_coverage").at("acyclic").get<bool>())
        result.source =
            SGLangTemplateBindings::produce(manifest_path, result.manifest, extractor.graph(), extractor.subgraphs(), extractor.calls(), extractor.facts());
    return result;
}

FrameworkConfiguration SGLangTemplateAdapter::source_configuration(const nlohmann::json & parameters) const {
    return SGLangConfiguration::from_parameters(parameters);
}

bool SGLangTemplateAdapter::shares_execution_path(const FrameworkConfiguration & source, const FrameworkConfiguration & target) const {
    const auto & source_configuration = std::get<SGLangConfiguration>(source);
    const auto & target_configuration = std::get<SGLangConfiguration>(target);
    const SGLangBindings bindings;

    // Pool capacity and Prefill budget select target work through the same
    // control path. Per-unit work matching remains the cost model's job.
    return bindings.control_limits(source_configuration).has_value() && bindings.control_limits(target_configuration).has_value()
           && bindings.supports_configuration_change(source_configuration, target_configuration);
}

std::expected<ServingLimits, std::string_view> SGLangTemplateAdapter::control_limits(const FrameworkConfiguration & configuration) const {
    return SGLangBindings{}.control_limits(std::get<SGLangConfiguration>(configuration));
}

TemplateSourceSelection SGLangTemplateAdapter::select_sources(const Candidate & candidate, std::span<const TemplateSourceCandidate> sources) const {
    const auto & configuration = std::get<SGLangConfiguration>(candidate.configuration);
    TemplateSourceSelection selection;
    bool deployment_available = false;
    for (const auto & candidate_source : sources) {
        const auto * source_configuration = std::get_if<SGLangConfiguration>(candidate_source.configuration);
        if (!source_configuration) continue;

        const auto * bank = candidate_source.bank;
        const auto & source = bank->source();
        if (source.deployment.service != candidate.environment.service || source.deployment.devices != candidate.environment.devices) continue;

        deployment_available = true;
        if (!shares_execution_path(*candidate_source.configuration, candidate.configuration)) continue;

        // A locally reusable operation does not grant the source's whole pool
        // or initialization state permission to migrate to the target.
        if (bank->qualify(source_configuration->max_running_requests)) {
            for (const auto & [key, definitions] : source.definitions)
                for (const auto & definition : definitions)
                    if (std::ranges::any_of(definition.batch_operations, [](const auto & entry) { return entry.second.request_pool_rebindable; }))
                        selection.operation_sources.push_back(&definition);
        }

        const auto qualification = bank->qualify(configuration.max_running_requests);
        if (!qualification) {
            if (!selection.gap) selection.gap = qualification.error();
            continue;
        }
        selection.donors.push_back(bank);

        // A usable donor need not supply the target's initial workspace state.
        const auto initialization = bank->qualify_initialization(configuration.max_running_requests);
        if (!initialization) {
            if (!selection.gap) selection.gap = initialization.error();
            continue;
        }
        selection.initial_sources.push_back(bank);
    }

    if (!selection.initial_sources.empty()) selection.gap.reset();
    else if (!selection.gap) {
        selection.gap = TemplateGap{ deployment_available ? TemplateGapKind::Rule : TemplateGapKind::Observation,
                                     deployment_available ? "parameter_change_outside_admission_rule" : "compatible_template_missing",
                                     StepKind::Schedule,
                                     BatchPhase::Idle,
                                     0 };
    }
    return selection;
}

nlohmann::json SGLangTemplateAdapter::candidate_recipe(const SGLangConfiguration & configuration) const {
    if (configuration.max_running_requests < 2) {
        return {
            { "status",                         "not_ready" },
            { "reason", "single_request_recipe_not_defined" }
        };
    }
    if (configuration.max_running_requests > SGLangTemplateBindings::last_location_concat_limit) {
        return {
            { "status",                                 "not_ready" },
            { "reason", "queued_recipe_exceeds_bound_concat_branch" }
        };
    }

    // These are mechanism triggers, not target workloads or duration samples.
    // Resolve P from the actual worker; do not copy a backend default here.
    using Json = nlohmann::json;
    auto groups = Json::array();
    groups.push_back({
        {             "purpose", "prefill_direct_completion" },
        { "prompt_page_offsets",     Json::array({ -2, -1 }) },
        {       "output_tokens",       Json::array({ 1, 1 }) },
        { "expected_mechanisms",          prefill_mechanisms }
    });
    groups.push_back({
        {             "purpose", "decode_page_crossing_and_partial_completion" },
        { "prompt_page_offsets",                        Json::array({ -1, 0 }) },
        {       "output_tokens",                         Json::array({ 4, 2 }) },
        { "expected_mechanisms",                 partial_completion_mechanisms }
    });
    groups.push_back({
        {             "purpose",   "decode_joint_completion" },
        { "prompt_page_offsets",     Json::array({ -2, -1 }) },
        {       "output_tokens",       Json::array({ 2, 2 }) },
        { "expected_mechanisms", joint_completion_mechanisms }
    });

    // Fill the native request pool, then leave one request queued. One member
    // finishes early; the queued singleton joins the still-running members.
    // Arrival timing is not guessed, and no native scheduling state is changed.
    const std::uint64_t capacity = configuration.max_running_requests;
    auto queued_offsets = Json::array();
    auto queued_outputs = Json::array();
    for (std::uint64_t member = 0; member < capacity; ++member) {
        queued_offsets.push_back(-1);
        queued_outputs.push_back(member + 1 == capacity ? 3 : 5);
    }
    queued_offsets.push_back(-1);
    queued_outputs.push_back(2);
    groups.push_back({
        {             "purpose", "queued_singleton_and_running_merge" },
        { "prompt_page_offsets",            std::move(queued_offsets) },
        {       "output_tokens",            std::move(queued_outputs) },
        { "expected_mechanisms",          queued_admission_mechanisms }
    });

    // Admission rounds each prompt to P and reserves alignment/output headroom.
    // Four pages per member conservatively cover this short recipe at P >= 4;
    // this is a collection precondition, not a target KV allocation formula.
    return {
        {                       "status",                                   "requires_asset_coverage"                                         },
        {             "page_size_source",                                                                    "effective_server_args.page_size" },
        {     "minimum_page_size_tokens",                                                                                                    4 },
        {       "minimum_prefill_budget",                                       { { "page_size_multiple", capacity }, { "offset_tokens", 0 } } },
        { "minimum_total_token_capacity",                                   { { "page_size_multiple", 4 * capacity }, { "offset_tokens", 0 } } },
        {                "request_count",                                                                                         capacity + 7 },
        {           "output_token_count",                                                                                    5 * capacity + 12 },
        {                   "submission",                               "submit_each_group_together_and_wait_for_completion_before_next_group" },
        {                       "groups",                                                                                    std::move(groups) },
        {         "uncovered_mechanisms",
         Json::array({ "periodic_nonfinal_output", "admission_budget_stop", "memory_pressure", "backend_variants_outside_qualified_domains" }) },
        {                   "completion",                    "all_expected_mechanisms_observed_on_every_rank_and_template_relations_qualified" },
        {      "missing_evidence_action",                                                            "stop_and_report_without_automatic_retry" }
    };
}

nlohmann::json SGLangTemplateAdapter::configurations(std::span<const Candidate * const> candidates) const {
    std::vector<ConfigurationGroup> groups;
    auto excluded = nlohmann::json::array();
    for (const auto * input : candidates) {
        const auto & candidate = *input;
        const auto & configuration = std::get<SGLangConfiguration>(candidate.configuration);
        const auto control = control_limits(candidate.configuration);
        std::string_view reason;
        if (!control) reason = control.error();
        else if (candidate.environment.devices.size() != configuration.tp_size) reason = "deployment_rank_count_mismatch";
        if (!reason.empty()) {
            excluded.push_back({
                { "configuration_id", candidate.id },
                {           "reason",       reason }
            });
            continue;
        }

        ConfigurationGroup * selected = nullptr;
        for (auto & group : groups) {
            const auto & source = *group.representative;
            if (source.environment.service != candidate.environment.service || source.environment.devices != candidate.environment.devices) continue;
            if (!shares_execution_path(source.configuration, candidate.configuration)) continue;
            selected = &group;
            break;
        }
        if (!selected) {
            groups.push_back({ &candidate, { &candidate } });
            continue;
        }

        selected->members.push_back(&candidate);
        // The current pool transform can shrink a captured table but cannot grow
        // one. At equal capacity, prefer prefill headroom for the capture recipe.
        const auto & source_configuration = std::get<SGLangConfiguration>(selected->representative->configuration);
        if (configuration.max_running_requests > source_configuration.max_running_requests
            || (configuration.max_running_requests == source_configuration.max_running_requests
                && configuration.max_prefill_tokens > source_configuration.max_prefill_tokens)) {
            selected->representative = &candidate;
        }
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
        {                  "status",          groups.empty() ? "unsupported" : "requires_mechanism_coverage"                                    },
        {                  "groups",                                                                                                      result },
        { "excluded_configurations",                                                                                                    excluded },
        {         "selection_basis",
         "same deployment and implemented configuration relation; largest request pool, then largest prefill budget; declaration order for ties" }
    };
}

} // namespace markov
