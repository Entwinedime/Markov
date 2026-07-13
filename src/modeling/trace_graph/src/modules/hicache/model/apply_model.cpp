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
    struct OrderedHiCacheNode {
        size_t node_id = 0;
        uint64_t boundary_ts = 0;
    };
    struct RoutedHiCacheFact {
        HiCacheFact fact;
        HiCacheFactRoute route;
        std::vector<std::string> required_errors;
    };

    HiCacheModelResult result;
    HiCacheSummary summary;
#ifdef DEBUG
    summary.target_config = config;
#endif

    HiCacheFactParser parser;
    std::vector<OrderedHiCacheNode> hicache_nodes;
    for (const auto & node : graph.nodes()) {
        const auto & event = graph.event_for_node(node.id);
        if (!parser.is_hicache_event(event)) continue;
        hicache_nodes.push_back(OrderedHiCacheNode{
            .node_id = node.id,
            .boundary_ts = hicache_fact_boundary_timestamp(event),
        });
        parser.observe_token_dictionaries(event);
    }
    std::ranges::sort(hicache_nodes, [&](const OrderedHiCacheNode & left, const OrderedHiCacheNode & right) {
        const auto & lhs = graph.event_for_node(left.node_id);
        const auto & rhs = graph.event_for_node(right.node_id);
        if (left.boundary_ts != right.boundary_ts) return left.boundary_ts < right.boundary_ts;
        if (lhs.pid != rhs.pid) return lhs.pid < rhs.pid;
        if (lhs.tid != rhs.tid) return lhs.tid < rhs.tid;
        if (lhs.name != rhs.name) return lhs.name < rhs.name;
        if (lhs.index != rhs.index) return lhs.index < rhs.index;
        return left.node_id < right.node_id;
    });

    HiCacheState state(config);
#ifdef DEBUG
    summary.resolved_policy = state.resolved_policy();
#endif
    std::vector<RoutedHiCacheFact> routed_facts;
    routed_facts.reserve(hicache_nodes.size());
    for (const auto & ordered_node : hicache_nodes) {
        const auto node_id = ordered_node.node_id;
        const auto & event = graph.event_for_node(node_id);
#ifdef DEBUG
        (void)core::checked_increment_u64(summary.input_hicache_events, "HiCache input event count exceeds uint64 range");
#endif
        auto fact = parser.parse(node_id, event);
#ifdef DEBUG
        (void)core::checked_increment_u64(summary.events_by_role[fact.role], "HiCache input role count exceeds uint64 range");
#endif

        auto route = route_hicache_fact(fact);
        auto required_errors = route.model_fact && route.known_role ? hicache_required_fact_errors(fact, route.role) : std::vector<std::string>{};
        routed_facts.push_back(RoutedHiCacheFact{
            .fact = std::move(fact),
            .route = route,
            .required_errors = std::move(required_errors),
        });
    }

    for (const auto & routed : routed_facts) {
        if (routed.route.model_fact && routed.route.known_role && routed.required_errors.empty() && routed.route.role == HiCacheFactRole::CacheExtendInput)
            state.register_prefetch_control_boundary(routed.fact);
    }

    for (const auto & routed : routed_facts) {
        const auto & fact = routed.fact;
        const auto & route = routed.route;
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
            (void)core::checked_increment_u64(result.effect_decisions.missing_facts["unknown_state_model_fact"],
                                              "HiCache effect-decision missing-fact count exceeds uint64 range");
            continue;
        }
        if (!routed.required_errors.empty()) {
#ifdef DEBUG
            std::ranges::for_each(routed.required_errors, [&](const auto & error) {
                (void)core::checked_increment_u64(summary.missing_state_model_facts[error], "HiCache missing state-model fact count exceeds uint64 range");
            });
#endif
            std::ranges::for_each(routed.required_errors, [&](const auto & error) {
                (void)core::checked_increment_u64(result.effect_decisions.missing_facts[error],
                                                  "HiCache effect-decision missing-fact count exceeds uint64 range");
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
    auto effect_decisions = state.effect_decision_ledger();
    for (const auto & [reason, count] : result.effect_decisions.missing_facts) {
        auto & merged_count = effect_decisions.missing_facts[reason];
        merged_count = core::checked_add_u64(merged_count, count, "HiCache merged missing-fact count exceeds uint64 range");
    }
    if (!effect_decisions.missing_facts.empty()) effect_decisions.status = "partial";
    result.effect_decisions = std::move(effect_decisions);
    result.replay_complete = true;
#ifdef DEBUG
    result.summary = std::move(summary);
#endif
    return result;
}

} // namespace markov::trace_graph::modules::hicache::model
