#include "markov/adapters/frameworks/sglang/template_adapter.hpp"

#include "markov/adapters/frameworks/sglang/structure_observation.hpp"
#include "markov/execution/structure_comparison.hpp"

#include "markov/adapters/frameworks/sglang/bindings.hpp"
#include "markov/adapters/frameworks/sglang/template_bindings.hpp"
#include "markov/adapters/runtimes/ascend/source_extractor.hpp"
#include "markov/contracts/environment.hpp"
#include "markov/contracts/json_input.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <stdexcept>
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

// Unknown target hardware means analysis under the source's conditions. A
// declared device/runtime requirement must stay with its own representative.
nlohmann::json device_requirement(const Environment & environment) {
    const auto & conditions = environment.measurement;
    if (conditions.is_null() || conditions.at("device").is_null()) return nullptr;
    return {
        {  "device",  conditions.at("device") },
        { "runtime", conditions.at("runtime") }
    };
}

} // namespace

TemplateProduction SGLangTemplateAdapter::produce(const std::filesystem::path & manifest_path, nlohmann::json manifest,
                                                  const std::filesystem::path & work_directory) const {
    // One input snapshot owns framework configuration throughout production.
    const auto prepared = input::read_json(input::resolve_path(manifest_path, manifest.at("input")));
    if (!prepared.contains("shard_execution")) throw std::invalid_argument("base template production requires a single-device shard capture");
    // Framework bindings outlive runtime extraction and its borrowed block declarations.
    const SGLangBindings bindings;
    AscendSourceExtractor extractor(bindings);
    TemplateProduction result{ extractor.extract(manifest_path, std::move(manifest), work_directory, prepared.at("diagnostics").get<bool>()), std::nullopt };
    // Required serving ports are checked by the extractor. An incomplete cut
    // remains an observation gap; it cannot enter executable template binding.
    if (result.manifest.at("graph_coverage").at("acyclic").get<bool>() && extractor.subgraphs().gaps.empty())
        result.source = SGLangTemplateBindings::produce(manifest_path,
                                                        result.manifest,
                                                        prepared,
                                                        extractor.graph(),
                                                        extractor.subgraphs(),
                                                        extractor.calls(),
                                                        extractor.facts());
    return result;
}

nlohmann::json SGLangTemplateAdapter::compare_structure(const std::filesystem::path & target_graph, nlohmann::json facts,
                                                        const nlohmann::json & environment) const {
    // The same one-shot fact reader serves production and observation. No
    // template or cost asset is produced from independent validation facts.
    const SGLangBindings bindings;
    AscendSourceExtractor source(bindings);
    source.read(std::move(facts));
    const SGLangStructureObservation semantics(source, bindings, bindings.request_capacity(environment));
    return StructureComparison(source.graph(), source.calls(), source.subgraphs(), semantics).run(target_graph);
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
    bool implementation_available = false;
    for (const auto & candidate_source : sources) {
        const auto * source_configuration = std::get_if<SGLangConfiguration>(candidate_source.configuration);
        if (!source_configuration) continue;

        const auto * bank = candidate_source.bank;
        const auto & source = bank->source();
        if (!source.deployment.shares_implementation(candidate.environment)) continue;

        if (!shares_execution_path(*candidate_source.configuration, candidate.configuration)) continue;
        const auto & target_environment = candidate.environment.measurement;
        if (!target_environment.is_null() && !target_environment.at("device").is_null()
            && !source.deployment.shares_measurement(candidate.environment, MeasurementScope::Device))
            continue;
        implementation_available = true;

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

    // An initial source is an entry-role anchor plus independently qualified
    // initial state for every role it binds. Source batch numbers are local to
    // each capture and play no part in joining these sessions.
    std::erase_if(selection.initial_sources, [&](const auto * anchor) {
        if (anchor->source().source_rank != anchor->source().rank_bindings.front().source_rank) return true;
        for (const auto & binding : anchor->source().rank_bindings) {
            const auto donor = std::ranges::find_if(selection.donors, [&](const auto * bank) {
                return bank->source().source_rank == binding.source_rank && anchor->shares_source_contract(*bank)
                       && bank->qualify_initialization(configuration.max_running_requests).has_value();
            });
            if (donor != selection.donors.end()) continue;
            selection.gap = TemplateGap{ TemplateGapKind::Observation,
                                         "native_worker_role_initial_state_not_observed",
                                         StepKind::Schedule,
                                         BatchPhase::Idle,
                                         binding.source_rank };
            return true;
        }
        return false;
    });

    if (!selection.initial_sources.empty()) selection.gap.reset();
    else if (!selection.gap) {
        selection.gap = TemplateGap{ implementation_available ? TemplateGapKind::Rule : TemplateGapKind::Observation,
                                     implementation_available ? "parameter_change_outside_admission_rule" : "compatible_template_missing",
                                     StepKind::Schedule,
                                     BatchPhase::Idle,
                                     0 };
    }
    return selection;
}

nlohmann::json SGLangTemplateAdapter::candidate_recipe(const SGLangConfiguration & configuration) const {
    if (configuration.max_running_requests > SGLangTemplateBindings::last_location_concat_limit) {
        return {
            { "status",                                 "not_ready" },
            { "reason", "queued_recipe_exceeds_bound_concat_branch" }
        };
    }

    // These are mechanism triggers, not target workloads or duration samples.
    // Resolve P from the actual worker; do not copy a backend default here.
    using Json = nlohmann::json;
    const std::uint64_t capacity = configuration.max_running_requests;
    const auto members = std::min<std::uint64_t>(capacity, 2);
    auto direct_offsets = Json::array();
    auto direct_outputs = Json::array();
    auto decode_offsets = Json::array();
    auto decode_outputs = Json::array();
    for (std::uint64_t member = 0; member < members; ++member) {
        direct_offsets.push_back(member == 0 ? -2 : -1);
        direct_outputs.push_back(1);
        decode_offsets.push_back(member == 0 ? -1 : 0);
        decode_outputs.push_back(member == 0 ? 4 : 2);
    }

    auto groups = Json::array();
    groups.push_back({
        {             "purpose", "prefill_direct_completion" },
        { "prompt_page_offsets",   std::move(direct_offsets) },
        {       "output_tokens",   std::move(direct_outputs) }
    });
    groups.push_back({
        {             "purpose", "decode_page_crossing_and_completion" },
        { "prompt_page_offsets",             std::move(decode_offsets) },
        {       "output_tokens",             std::move(decode_outputs) }
    });
    if (members > 1)
        groups.push_back({
            {             "purpose", "decode_joint_completion" },
            { "prompt_page_offsets",   Json::array({ -2, -1 }) },
            {       "output_tokens",     Json::array({ 2, 2 }) }
        });

    // Fill the pool and leave one request queued. A singleton pool drains
    // before readmission; larger pools also exercise the running-batch merge.
    auto queued_offsets = Json::array();
    auto queued_outputs = Json::array();
    for (std::uint64_t member = 0; member < capacity; ++member) {
        queued_offsets.push_back(-1);
        queued_outputs.push_back(member + 1 == capacity ? 3 : 5);
    }
    queued_offsets.push_back(-1);
    queued_outputs.push_back(2);
    groups.push_back({
        {             "purpose",        "queued_admission" },
        { "prompt_page_offsets", std::move(queued_offsets) },
        {       "output_tokens", std::move(queued_outputs) }
    });

    std::size_t request_count = 0;
    std::uint64_t output_token_count = 0;
    for (const auto & group : groups) {
        request_count += group.at("prompt_page_offsets").size();
        for (const auto & output : group.at("output_tokens")) output_token_count += output.get<std::uint64_t>();
    }

    // Admission rounds each prompt to P and reserves alignment/output headroom.
    // Four pages per member conservatively cover this short recipe at P >= 4;
    // this is a collection precondition, not a target KV allocation formula.
    return {
        {                       "status",                                   "requires_asset_coverage"                                         },
        {             "page_size_source",                                                                    "effective_server_args.page_size" },
        {     "minimum_page_size_tokens",                                                                                                    4 },
        {       "minimum_prefill_budget",                                       { { "page_size_multiple", capacity }, { "offset_tokens", 0 } } },
        { "minimum_total_token_capacity",                                   { { "page_size_multiple", 4 * capacity }, { "offset_tokens", 0 } } },
        {                "request_count",                                                                                        request_count },
        {           "output_token_count",                                                                                   output_token_count },
        {                   "submission",                               "submit_each_group_together_and_wait_for_completion_before_next_group" },
        {                       "groups",                                                                                    std::move(groups) },
        {         "uncovered_mechanisms",
         Json::array({ "periodic_nonfinal_output", "admission_budget_stop", "memory_pressure", "backend_variants_outside_qualified_domains" }) },
        {                   "completion",                             "native_groups_finished; actual_template_rules_qualify_target_instances" },
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
            if (source.environment.image_id != candidate.environment.image_id) continue;
            if (device_requirement(source.environment) != device_requirement(candidate.environment)) continue;
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
        {                  "status",groups.empty() ? "unsupported" : "requires_mechanism_coverage"                                    },
        {                  "groups",                                                         result },
        { "excluded_configurations",                                                       excluded },
        {         "selection_basis",
         "same immutable implementation, declared device/runtime requirements and implemented configuration relation; largest request pool, then largest "
         "prefill budget; declaration order for "
         "ties"                                                                                    }
    };
}

} // namespace markov
