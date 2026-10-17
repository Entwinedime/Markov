#include "markov/workflow/performance_requirements_plan.hpp"

#include "markov/contracts/control.hpp"
#include "markov/contracts/json_input.hpp"
#include "markov/contracts/request.hpp"
#include "markov/performance/cost_query.hpp"
#include "markov/templates/templates.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace markov {
namespace {

using Json = nlohmann::json;

Json kernel_parameters(const TemplateDefinition & definition, const TemplateNodeObservation & observation) {
    const auto span = observation.source_span;
    const auto kv = definition.kv_kernel_layouts.find(span);
    if (kv != definition.kv_kernel_layouts.end())
        return kv->second ? kv->second->describe_parameters()
                          : Json{
                                {         "rule",               "kv" },
                                { "contract_gap", kv->second.error() }
        };

    const auto batch = definition.batch_kernel_layouts.find(span);
    if (batch != definition.batch_kernel_layouts.end())
        return batch->second ? batch->second->describe_parameters()
                             : Json{
                                   {         "rule",               "batch" },
                                   { "contract_gap", batch->second.error() }
        };

    const auto attention = definition.attention_kernel_layouts.find(span);
    if (attention != definition.attention_kernel_layouts.end()) return attention->second.rule->describe_parameters();
    const auto model = definition.model_kernel_layouts.find(span);
    if (model != definition.model_kernel_layouts.end()) return definition.graph.model_rule(model->second.node).describe_parameters();

    const auto pool = definition.request_pool_layouts.find(span);
    if (pool != definition.request_pool_layouts.end())
        return pool->second ? pool->second->describe_parameters()
                            : Json{
                                  {         "rule",       "request_pool" },
                                  { "contract_gap", pool->second.error() }
        };
    return {
        {      "rule",   "unchanged_source_descriptor" },
        { "condition", "containing_template_selection" }
    };
}

CostQuery source_work(const TemplateNode & node, const TemplateNodeObservation & observation) {
    return CostQuery::from_work({ node.kind,
                                  observation.kernel,
                                  observation.communication,
                                  observation.transfer,
                                  observation.device_command ? &*observation.device_command : nullptr });
}

// Group by named measurement object, then preserve each source prototype and
// its rule together. Different source shapes never become target sample points.
class ObjectInventory {
public:
    void add(const CostQuery & query, const TemplateDefinition & definition, const Json & parameters) {
        const ServingContract contract;
        for (const auto & object : query.objects) {
            auto description = object.describe();
            Json identity{
                {                 "kind",                 description.at("kind") },
                {                 "name",                 description.at("name") },
                { "measurement_boundary", description.at("measurement_boundary") }
            };
            auto & entry = objects_[identity.dump()];
            entry.identity = std::move(identity);

            Json prototype{
                {                          "step",   contract.block(definition.step).name },
                {                         "phase",  contract.phase_name(definition.phase) },
                {     "source_prototype_features",     std::move(description["features"]) },
                {     "source_prototype_contract",     std::move(description["contract"]) },
                { "source_prototype_contract_gap", std::move(description["contract_gap"]) },
                {            "parameter_relation",                             parameters }
            };
            if (definition.length_domain) {
                const auto & domain = *definition.length_domain;
                prototype["template_length_condition"] = {
                    {   "minimum_input_tokens",   domain.minimum_input_tokens },
                    {   "maximum_input_tokens",   domain.maximum_input_tokens },
                    { "minimum_context_tokens", domain.minimum_context_tokens },
                    { "maximum_context_tokens", domain.maximum_context_tokens }
                };
            }

            auto & variant = entry.variants[prototype.dump()];
            variant.description = std::move(prototype);
            variant.sources.insert(definition.source_manifest.string());
            ++variant.observations;
        }
    }

    Json describe() const {
        auto objects = Json::array();
        for (const auto & [key, entry] : objects_) {
            auto row = entry.identity;
            auto variants = Json::array();
            for (const auto & [variant_key, variant] : entry.variants) {
                auto description = variant.description;
                description["source_manifests"] = variant.sources;
                description["source_occurrences"] = variant.observations;
                variants.push_back(std::move(description));
            }
            row["source_prototype_variants"] = std::move(variants);
            row["target_points"] = "unresolved_until_instantiation";
            objects.push_back(std::move(row));
        }
        return objects;
    }

private:
    struct Variant {
        Json description;
        std::set<std::string> sources;
        std::size_t observations = 0;
    };
    struct Object {
        Json identity;
        std::map<std::string, Variant> variants;
    };
    std::map<std::string, Object> objects_;
};

void collect_definition(ObjectInventory & inventory, const TemplateDefinition & definition) {
    std::map<std::size_t, Json> operation_conditions;
    for (const auto & operation : definition.operations) {
        const auto variant = definition.batch_operations.find(operation.host.source_span);
        if (variant == definition.batch_operations.end()) continue;

        // Reuse the same owned region that operation replacement consumes.
        // Its stored branch conditions remain separate from tensor-axis rules.
        const auto condition = variant->second.describe_parameters();
        for (const auto node : definition.operation_region(operation).nodes) {
            operation_conditions.try_emplace(node, Json::array()).first->second.push_back(condition);
        }
    }

    std::map<SourceSpanId, Json> communication_parameters;
    for (const auto & collective : definition.collectives) {
        const auto & rule = definition.graph.collective_rule(collective.envelope_node);
        Json parameters = rule.work ? rule.work->describe_parameters()
                                    : Json{
                                          {         "rule",   "communication" },
                                          { "contract_gap", rule.work.error() }
        };
        if (rule.count)
            parameters["element_count"] = {
                {     "extent", rule.count->rows == CollectiveRows::Tokens ? "input_tokens" : "requests" },
                { "multiplier",                                                        rule.count->width }
            };
        else parameters["count_relation"] = "unresolved";
        for (std::size_t task = 0; task < collective.observation.tasks.size(); ++task) {
            auto relation = parameters;
            relation["task_slot"] = task;
            communication_parameters.emplace(collective.observation.tasks[task].span, std::move(relation));
        }
    }

    for (std::size_t index = 0; index < definition.graph.size(); ++index) {
        const auto & node = definition.graph[index];
        if (node.role() != TemplateNodeRole::Service) continue;
        const auto & observation = definition.observation(node);
        Json parameters{
            {      "rule",              "retained_service" },
            { "condition", "containing_template_selection" }
        };
        if (observation.kernel) parameters = kernel_parameters(definition, observation);
        else if (observation.communication) {
            const auto communication = communication_parameters.find(observation.source_span);
            if (communication != communication_parameters.end()) parameters = communication->second;
        }
        else if (observation.transfer) {
            const auto transfer = definition.transfer_layouts.find(observation.source_span);
            if (transfer != definition.transfer_layouts.end()) parameters = transfer->second.describe_parameters();
        }
        if (const auto conditions = operation_conditions.find(index); conditions != operation_conditions.end())
            parameters["batch_operation_conditions"] = conditions->second;
        inventory.add(source_work(node, observation), definition, parameters);
    }
}

Json workload_bounds(std::span<const Request> requests, const ServingLimits & limits, std::uint32_t page_size) {
    std::vector<std::uint64_t> prompts;
    std::uint64_t maximum_sequence = 0;
    std::uint64_t maximum_output = 0;
    bool resolved = true;
    for (const auto & request : requests) {
        maximum_output = std::max(maximum_output, static_cast<std::uint64_t>(request.output_tokens));
        const auto * tokens = std::get_if<std::vector<std::uint32_t>>(&request.input);
        if (!tokens) {
            resolved = false;
            continue;
        }
        prompts.push_back(tokens->size());
        maximum_sequence = std::max(maximum_sequence, static_cast<std::uint64_t>(tokens->size()) + request.output_tokens - 1);
    }
    std::ranges::sort(prompts, std::greater<>());
    const auto batch_maximum = std::min<std::size_t>(requests.size(), limits.request_capacity);
    std::uint64_t prefill_maximum = 0;
    // Admission checks the input budget after accepting a request. The final
    // member may exceed it; the largest B prompts remain a safe upper bound.
    for (std::size_t index = 0; index < std::min(batch_maximum, prompts.size()); ++index) prefill_maximum += prompts[index];

    return {
        {                         "kind",                     "conservative_workload_and_configuration_envelope" },
        {       "token_lengths_resolved",                                                               resolved },
        {                "request_count",                                                        requests.size() },
        {       "batch_requests_maximum",                                                          batch_maximum },
        { "prefill_input_tokens_maximum",                       resolved ? Json(prefill_maximum) : Json(nullptr) },
        {  "decode_input_tokens_maximum",                     maximum_output > 1 ? Json(batch_maximum) : Json(0) },
        {      "maximum_sequence_tokens",                      resolved ? Json(maximum_sequence) : Json(nullptr) },
        {     "generated_tokens_maximum",                                                         maximum_output },
        {            "request_pool_rows",                static_cast<std::uint64_t>(limits.request_capacity) + 1 },
        {           "kv_capacity_tokens",                                              limits.kv_capacity_tokens },
        {             "kv_pages_maximum",                                  limits.kv_capacity_tokens / page_size },
        {      "prefill_capacity_tokens",                                         limits.prefill_capacity_tokens },
        {             "page_size_tokens",                                                              page_size },
        {               "interpretation", "upper bounds only; no Cartesian reachability or model coverage claim" }
    };
}

} // namespace

PerformanceRequirementsPlan::PerformanceRequirementsPlan(std::filesystem::path output_directory)
    : output_directory_(std::filesystem::absolute(output_directory)) {}

nlohmann::json PerformanceRequirementsPlan::run(const TemplateBank & primary, std::span<const TemplateBank * const> donors,
                                                std::span<const TemplateDefinition * const> operation_sources, std::span<const Request> requests,
                                                const ServingLimits & limits) {
    std::set<const TemplateDefinition *> definitions;
    std::set<const TemplateBank *> banks{ &primary };
    banks.insert(donors.begin(), donors.end());
    for (const auto * bank : banks)
        for (const auto & [key, source_definitions] : bank->source().definitions)
            for (const auto & definition : source_definitions) definitions.insert(&definition);
    definitions.insert(operation_sources.begin(), operation_sources.end());

    std::vector<const TemplateDefinition *> selected(definitions.begin(), definitions.end());
    const auto existing = std::ranges::find(catalogs_, selected, &Catalog::definitions);
    std::filesystem::path catalog_path;
    if (existing != catalogs_.end()) catalog_path = existing->path;
    else {
        ObjectInventory inventory;
        for (const auto * definition : selected) collect_definition(inventory, *definition);
        const Json catalog{
            {                 "objects",               inventory.describe()                                        },
            { "source_definition_count",                                                            selected.size() },
            {        "extent_relations",
             { { "input_tokens", "sum(target_request.input_tokens)" },
             { "requests", "target_batch.requests.size" },
             { "maximum_sequence_tokens", "max(target_request.context_tokens+target_request.input_tokens)" },
             { "page_columns", "ceil(maximum_sequence_tokens/page_size_tokens)" },
             { "sequence_storage_elements", "requests*maximum_sequence_tokens" },
             { "request_pool_rows", "request_capacity+1" } }                                                       },
            {      "dynamic_unresolved",
             { "timing_dependent_batch_membership",
             "filter_merge_and_completion_membership",
             "kv_and_workspace_state",
             "actual_node_features_and_launch_applicability" }                                                     },
            {                "coverage", "source prototypes and relations do not establish target feature coverage" }
        };

        // Catalogs belong to the run, not a prediction round. Compare complete
        // content so a changed relation never silently reuses an older contract.
        // The parent workflow serializes writers to this output directory.
        std::filesystem::create_directories(output_directory_);
        for (std::size_t index = 0;; ++index) {
            catalog_path = output_directory_ / ("performance-objects_" + std::to_string(index) + ".json");
            if (std::filesystem::exists(catalog_path)) {
                if (input::read_json(catalog_path) == catalog) break;
                continue;
            }

            auto pending = catalog_path;
            pending += ".tmp";
            std::ofstream output;
            output.exceptions(std::ios::failbit | std::ios::badbit);
            output.open(pending);
            output << catalog << '\n';
            output.close();
            std::filesystem::rename(pending, catalog_path);
            break;
        }
        catalogs_.push_back({ std::move(selected), catalog_path });
    }

    return {
        { "status", "potential_requirements" },
        { "catalog", catalog_path.string() },
        { "target_bounds", workload_bounds(requests, limits, primary.page_size_tokens()) }
    };
}

} // namespace markov
