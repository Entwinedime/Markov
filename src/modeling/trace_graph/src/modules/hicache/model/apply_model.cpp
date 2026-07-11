/**
 * @file
 * @brief HiCache fact scanning, target-state replay, and Debug summary convergence.
 */
#include "markov/trace_graph/modules/hicache/model/state.hpp"

#include "markov/trace_graph/core/numeric.hpp"

#include <algorithm>
#include <ranges>

namespace markov::trace_graph::modules::hicache::model {

using core::DagGraph;
using frontend::HiCacheConfig;

/**
 * @brief Replays approved HiCache facts and emits a Debug module summary.
 *
 * The pass first indexes approved token dictionaries, then dispatches facts in stable DAG
 * node order, finalizes target-derived asynchronous lifecycles, and finally aggregates
 * diagnostics in Debug builds. State replay never consumes source-actual outcomes.
 */
HiCacheModelResult apply_hicache_model(DagGraph & graph, const HiCacheConfig & config) {
    HiCacheModelResult result;
    HiCacheSummary summary;
#ifdef DEBUG
    summary.target_config = config;
#endif

    HiCacheFactParser parser;
    std::vector<size_t> hicache_node_ids;
    for (const auto & node : graph.nodes()) {
        const auto & event = graph.event_for_node(node.id);
        if (!parser.is_hicache_event(event)) continue;
        hicache_node_ids.push_back(node.id);
        parser.observe_token_dictionaries(event);
    }

    HiCacheState state(config);
#ifdef DEBUG
    summary.resolved_policy = state.resolved_policy();
#endif
    for (const auto node_id : hicache_node_ids) {
        const auto & event = graph.event_for_node(node_id);
#ifdef DEBUG
        (void)core::checked_increment_u64(summary.input_hicache_events, "HiCache input event count exceeds uint64 range");
#endif
        auto fact = parser.parse(node_id, event);
#ifdef DEBUG
        (void)core::checked_increment_u64(summary.events_by_role[fact.role], "HiCache input role count exceeds uint64 range");
#endif

        auto route = route_hicache_fact(fact);
        if (!route.model_fact) {
#ifdef DEBUG
            (void)core::checked_increment_u64(summary.skipped_non_state_model_events, "HiCache skipped event count exceeds uint64 range");
#endif
            continue;
        }
        if (!route.known_role) {
#ifdef DEBUG
            (void)core::checked_increment_u64(summary.missing_state_model_facts["unknown_state_model_fact"],
                                              "HiCache missing state-model fact count exceeds uint64 range");
#endif
            (void)core::checked_increment_u64(result.effect_intents.missing_facts["unknown_state_model_fact"],
                                              "HiCache effect missing-fact count exceeds uint64 range");
            continue;
        }
        const auto required_errors = hicache_required_fact_errors(fact, route.role);
        if (!required_errors.empty()) {
#ifdef DEBUG
            std::ranges::for_each(required_errors, [&](const auto & error) {
                (void)core::checked_increment_u64(summary.missing_state_model_facts[error], "HiCache missing state-model fact count exceeds uint64 range");
            });
#endif
            std::ranges::for_each(required_errors, [&](const auto & error) {
                (void)core::checked_increment_u64(result.effect_intents.missing_facts[error], "HiCache effect missing-fact count exceeds uint64 range");
            });
            continue;
        }

        HiCacheTransitionBuffer transitions;
        state.apply_fact(fact, route.role, summary, transitions);
#ifdef DEBUG
        (void)core::checked_increment_u64(summary.processed_hicache_events, "HiCache processed event count exceeds uint64 range");
        (void)core::checked_increment_u64(summary.processed_events_by_role[hicache_fact_role_name(route.role)],
                                          "HiCache processed role count exceeds uint64 range");
        summary.transition_trace.insert(summary.transition_trace.end(), transitions.begin(), transitions.end());
        summary.state_transition_count = summary.transition_trace.size();
#endif
    }

    HiCacheTransitionBuffer final_transitions;
    state.finalize(summary, final_transitions);
#ifdef DEBUG
    summary.transition_trace.insert(summary.transition_trace.end(), final_transitions.begin(), final_transitions.end());
    summary.state_transition_count = summary.transition_trace.size();

    const auto final_state = state.derived_state(HiCacheDerivedStateMode::MaterializedOnly);
    summary.final_state_derivation_mode = hicache_derived_state_mode_name(final_state.mode);
    const auto inclusive_state = state.derived_state(HiCacheDerivedStateMode::StorageDirectoryInclusive);
    summary.storage_directory_inclusive_state = inclusive_state;
    summary.active_ref_owner_count = state.active_ref_owner_count();
    summary.radix_split_count = state.radix_split_count();
    summary.radix_split_trace = state.radix_split_trace();
    summary.control_boundary_count = state.control_boundary_count();
    summary.control_boundary_trace = state.control_boundary_trace();
    summary.async_lifecycle_transition_count = state.async_lifecycle_transition_count();
    summary.async_lifecycle_trace = state.async_lifecycle_trace();
    summary.policy_decision_count = state.policy_decision_count();
    summary.policy_decision_trace = state.policy_decision_trace();
    summary.storage_known_page_count = state.storage_known_page_count();
    summary.storage_readable_page_count = state.storage_readable_page_count();
    summary.storage_backend_readable_count = state.storage_backend_readable_count();
    summary.storage_materialized_page_count = state.storage_materialized_page_count();
    summary.capacity_mutation_count = state.capacity_mutation_count();
    summary.capacity_victim_choice_count = state.capacity_victim_choice_count();
    summary.capacity_mutation_trace = state.capacity_mutation_trace();
    summary.capacity_victim_choices = state.capacity_victim_choices();
    summary.capacity_audit_issues = state.capacity_audit_issues();
    summary.capacity_audit_issue_count = summary.capacity_audit_issues.size();
    summary.ref_mutation_count = state.ref_mutation_count();
    summary.ref_mutation_trace = state.ref_mutation_trace();
    summary.ref_audit_issues = state.ref_audit_issues();
    summary.ref_audit_issue_count = summary.ref_audit_issues.size();
    summary.l1_resident_pages = hicache_sorted_vector(final_state.l1);
    summary.l2_resident_pages = hicache_sorted_vector(final_state.l2);
    summary.l3_resident_pages = hicache_sorted_vector(final_state.l3);
    summary.dirty_pages = hicache_sorted_vector(final_state.dirty);
    summary.backuped_pages = hicache_sorted_vector(final_state.backuped);
    summary.evicted_pages = hicache_sorted_vector(final_state.evicted);
    summary.locked_pages = hicache_sorted_vector(final_state.locked);
    summary.pending_writeback_pages = hicache_sorted_vector(final_state.pending_writeback);
    summary.prefetch_planned_pages = hicache_sorted_vector(final_state.prefetch_planned);
    summary.prefetch_ready_pages = hicache_sorted_vector(final_state.prefetch_ready);
    summary.prefetch_late_pages = hicache_sorted_vector(final_state.prefetch_late);
    summary.prefetch_suppressed_pages = hicache_sorted_vector(final_state.prefetch_suppressed);
    summary.page_hit_counts = final_state.page_hit_counts;
    if (summary.dirty_eviction_events > 0)
        summary.warnings.push_back("write_back eviction used synchronous modeled writeback; ack timing is intentionally not modeled yet.");
    if (summary.capacity_audit_issue_count > 0)
        summary.warnings.push_back("HiCache capacity index audit found mismatches between mutation-driven index and canonical tree.");
    if (summary.ref_audit_issue_count > 0) summary.warnings.push_back("HiCache ref ledger audit found mismatches between owner ledger and tree ref counters.");
#endif
    auto effect_intents = state.effect_intent_catalog();
    for (const auto & [reason, count] : result.effect_intents.missing_facts) {
        auto & merged_count = effect_intents.missing_facts[reason];
        merged_count = core::checked_add_u64(merged_count, count, "HiCache merged missing-fact count exceeds uint64 range");
    }
    result.effect_intents = std::move(effect_intents);
    result.replay_complete = true;
#ifdef DEBUG
    result.summary = std::move(summary);
#endif
    return result;
}

} // namespace markov::trace_graph::modules::hicache::model
