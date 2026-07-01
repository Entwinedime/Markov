/**
 * @file
 * @brief HiCache state model 的输入扫描、state replay 和 Debug summary 收敛。
 */
#include "markov/trace_graph/modules/hicache/model/detail/state_model_helpers.hpp"

#include <algorithm>
#include <ranges>

namespace markov::trace_graph::modules::hicache::model {

using core::DagGraph;
using frontend::HiCacheConfig;

/**
 * @brief 对 DAG 中的 HiCache fact 执行 target state replay，并在 Debug 下生成 module summary。
 *
 * 流程分三步：
 * 1. 先观察 state-model path fact 的 token dictionary，让 span-only fact 能解析成 target page path；
 * 2. 按 DAG node 顺序 dispatch fact；
 * 3. 在 finalize 收敛 pending async lifecycle；
 * 4. 仅 Debug 构建聚合 diagnostics/validation summary。
 */
HiCacheSummary apply_hicache_model(DagGraph & graph, const HiCacheConfig & config) {
    HiCacheSummary summary;
#ifdef DEBUG
    summary.target_config = config;
    summary.resolved_policy = resolve_hicache_policy(config);
#endif

    HiCacheFactParser parser;
    for (const auto & node : graph.nodes()) {
        const auto & event = graph.event_for_node(node.id);
        parser.observe_token_dictionaries(event);
    }

    HiCacheState state(config);
    for (const auto & node : graph.nodes()) {
        const auto & event = graph.event_for_node(node.id);
        if (!parser.is_hicache_event(event)) continue;
#ifdef DEBUG
        summary.input_hicache_events++;
#endif
        auto fact = parser.parse(node.id, event);
#ifdef DEBUG
        summary.events_by_role[fact.role]++;
#endif

        auto route = route_hicache_fact(fact);
        if (!route.model_fact) {
#ifdef DEBUG
            summary.skipped_non_state_model_events++;
#endif
            continue;
        }
        if (!route.known_role || !hicache_fact_role_implemented(route.role)) {
#ifdef DEBUG
            summary.missing_state_model_facts["unknown_state_model_fact"]++;
#endif
            continue;
        }
        const auto required_errors = hicache_required_fact_errors(fact, route.role, effective_page_size(config, fact));
        if (!required_errors.empty()) {
#ifdef DEBUG
            std::ranges::for_each(required_errors, [&](const auto & error) { summary.missing_state_model_facts[error]++; });
#endif
            continue;
        }

        HiCacheTransitionBuffer transitions;
        state.apply_fact(fact, route.role, summary, transitions);
#ifdef DEBUG
        summary.processed_hicache_events++;
        summary.processed_events_by_role[hicache_fact_role_name(route.role)]++;
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
    return summary;
}

} // namespace markov::trace_graph::modules::hicache::model
