/**
 * @file
 * @brief Compact JSON projection of the target-derived HiCache effect plan.
 */
#include "markov/trace_graph/modules/hicache/diagnostics/summary.hpp"

#include <nlohmann/json.hpp>

namespace markov::trace_graph::modules::hicache::diagnostics {

namespace {

using Json = nlohmann::json;
using model::HiCacheEffectDecision;
using model::HiCacheEffectDecisionLedger;

Json page_ids(const std::vector<model::HiCacheEffectSegment> & segments) {
    Json pages = Json::array();
    for (const auto & segment : segments) pages.push_back(segment.target_page_id);
    return pages;
}

uint64_t service_operation_count(const HiCacheEffectDecision & effect) {
    if (effect.direction == model::HiCacheTransferDirection::StorageToHost
        || effect.direction == model::HiCacheTransferDirection::HostToStorage)
        return static_cast<uint64_t>(effect.storage_service_batches.size());
    if (effect.direction == model::HiCacheTransferDirection::HostToDevice
        || effect.direction == model::HiCacheTransferDirection::DeviceToHost)
        return effect.effective_page_count == 0 ? 0 : static_cast<uint64_t>(effect.operation_ids.size());
    return 0;
}

Json effect_json(const HiCacheEffectDecision & effect) {
    Json batches = Json::array();
    for (const auto & batch : effect.storage_service_batches) {
        batches.push_back({
            {"operation_index", batch.operation_index},
            {"page_count", batch.page_count},
            {"existing_page_count", batch.existing_page_count},
            {"new_page_count", batch.new_page_count},
        });
    }
    return {
        {              "effect_key",                                                         effect.effect_key },
        {       "effect_family_key",                                                  effect.effect_family_key },
        {             "effect_type",                         model::hicache_effect_type_name(effect.effect_type) },
        {               "direction",                 model::hicache_transfer_direction_name(effect.direction) },
        {             "cache_scope",                                                       effect.cache_scope },
        {        "source_fact_role",                                                  effect.source_fact_role },
        {     "source_fact_ordinal",                                               effect.source_fact_ordinal },
        {     "target_effect_state",           model::hicache_target_effect_state_name(effect.target_effect_state) },
        {    "schedule_sensitivity", model::hicache_schedule_sensitivity_name(effect.schedule_sensitivity) },
        { "consumer_dependency_required",                            effect.consumer_dependency_required },
        {   "policy_wait_duration_us",                              effect.policy_wait_duration_us },
        {            "resource_lane",                                                     effect.resource_lane },
        {       "eligibility_epoch",                                      effect.eligibility_boundary.epoch },
        {           "consumer_role",                               effect.consumer_boundary.source_fact_role },
        {         "candidate_pages",                                      page_ids(effect.candidate_segments) },
        {         "effective_pages",                                                   effect.effective_pages },
        {      "effective_page_count",                                        effect.effective_page_count },
        {      "completed_page_count",                                        effect.completed_page_count },
        {      "effective_byte_count",                                        effect.effective_byte_count },
        {            "operation_count",                                      service_operation_count(effect) },
        { "storage_existing_page_count",                               effect.storage_existing_page_count },
        {      "storage_new_page_count",                                    effect.storage_new_page_count },
        {     "storage_service_batches",                                                     std::move(batches) },
        {             "patch_status",                 model::hicache_effect_patch_status_name(effect.patch_status) },
        {                    "reason",                                                            effect.reason },
        {      "not_patchable_reason",                                              effect.not_patchable_reason },
    };
}

Json effect_plan_json(const HiCacheEffectDecisionLedger & plan) {
    Json effects = Json::array();
    for (const auto & effect : plan.decisions) effects.push_back(effect_json(effect));
    return {
        {                            "status",                                 plan.status },
        {                    "decision_count",                       plan.decisions.size() },
        {                     "patchable_count",                      plan.patchable_count() },
        {                 "not_patchable_count",                  plan.not_patchable_count() },
        {                      "deferred_count",                       plan.deferred_count() },
        {                    "unresolved_count",                     plan.unresolved_count() },
        {                 "decision_coverage",                      plan.decision_coverage },
        {             "counts_by_effect_type",            plan.counts_by_effect_type() },
        {     "counts_by_target_effect_state",    plan.counts_by_target_effect_state() },
        { "counts_by_schedule_sensitivity", plan.counts_by_schedule_sensitivity() },
        {                     "missing_facts",                      plan.missing_facts },
        {             "not_patchable_reasons",              plan.not_patchable_reasons },
        {                         "decisions",                          std::move(effects) },
    };
}

} // namespace

std::string summary_json(const HiCacheEffectDecisionLedger & effect_plan) {
    return Json{ { "effect_decisions", effect_plan_json(effect_plan) } }.dump();
}

} // namespace markov::trace_graph::modules::hicache::diagnostics
