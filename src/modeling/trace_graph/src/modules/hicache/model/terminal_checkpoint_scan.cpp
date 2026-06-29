/**
 * @file
 * @brief HiCache state model 的输入扫描、terminal checkpoint 预处理和 summary 收敛。
 */
#include "markov/trace_graph/modules/hicache/model/detail/state_model_helpers.hpp"

#include <algorithm>
#include <iterator>
#include <ranges>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace markov::trace_graph::modules::hicache::model {

using core::DagGraph;
using frontend::HiCacheConfig;

/**
 * @brief 预扫描每个 request-local prefetch 流程的最后一个 checkpoint。
 *
 * 这里只看当前 state model 可消费的 target-derived fact，不读取 source actual progress。
 * 新的 prefetch_decision 会关闭同 request 的上一段 prefetch 流程；最后一个
 * prefetch_check_point 被标记为 terminal，供 timeout/wait_complete policy 避免在普通
 * 中间 checkpoint 上过早 materialize。
 */
std::unordered_set<size_t> collect_terminal_prefetch_checkpoint_events(DagGraph & graph, const HiCacheConfig & config, const HiCacheFactParser & parser) {
    std::unordered_set<size_t> terminal_events;
    std::unordered_set<std::string> active_prefetch_keys;
    std::unordered_map<std::string, size_t> latest_checkpoint_by_key;

    auto close_prefetch_process = [&](const std::string & key) {
        if (const auto it = latest_checkpoint_by_key.find(key); it != latest_checkpoint_by_key.end()) {
            terminal_events.insert(it->second);
            latest_checkpoint_by_key.erase(it);
        }
    };

    for (const auto & node : graph.nodes()) {
        const auto & event = graph.event_for_node(node.id);
        if (!parser.is_hicache_event(event)) continue;

        const auto fact = parser.parse(node.id, event);
        const auto route = route_hicache_fact(fact);
        if (!ready_state_model_fact(fact, route, config)) continue;
        if (route.role != HiCacheFactRole::PrefetchDecision && route.role != HiCacheFactRole::PrefetchCheckPoint) continue;

        const auto key = scoped_fact_request_key(fact);
        if (key.empty()) continue;

        if (route.role == HiCacheFactRole::PrefetchDecision) {
            close_prefetch_process(key);
            active_prefetch_keys.insert(key);
            continue;
        }

        if (active_prefetch_keys.contains(key)) latest_checkpoint_by_key[key] = fact.source_event_index;
    }

    for (const auto & key : active_prefetch_keys) close_prefetch_process(key);
    return terminal_events;
}

/**
 * @brief 对 DAG 中的 HiCache fact 执行 target state replay，并生成 module summary。
 *
 * 流程分三步：
 * 1. 先观察 token dictionary，让 span-only fact 能解析成 target page path；
 * 2. 再预扫描 terminal prefetch checkpoint；
 * 3. 最后按 DAG node 顺序 dispatch fact，并在 finalize 收敛 pending lifecycle。
 */
HiCacheSummary apply_hicache_model(DagGraph & graph, const HiCacheConfig & config) {
    HiCacheSummary summary;
    summary.target_config = config;
    summary.resolved_policy = resolve_hicache_policy(config);

    HiCacheFactParser parser;
    for (const auto & node : graph.nodes()) {
        const auto & event = graph.event_for_node(node.id);
        parser.observe_token_dictionaries(event);
    }

    auto terminal_prefetch_checkpoint_events = collect_terminal_prefetch_checkpoint_events(graph, config, parser);
    HiCacheState state(config, std::move(terminal_prefetch_checkpoint_events));
    for (const auto & node : graph.nodes()) {
        const auto & event = graph.event_for_node(node.id);
        if (!parser.is_hicache_event(event)) continue;
        summary.input_hicache_events++;
        auto fact = parser.parse(node.id, event);
        summary.events_by_role[fact.role]++;

        auto route = route_hicache_fact(fact);
        if (!route.model_fact) {
            summary.skipped_non_state_model_events++;
            continue;
        }
        if (!route.known_role || !hicache_fact_role_implemented(route.role)) {
            summary.missing_state_model_facts["unknown_state_model_fact"]++;
            continue;
        }
        const auto required_errors = hicache_required_fact_errors(fact, route.role, effective_page_size(config, fact));
        if (!required_errors.empty()) {
            std::ranges::for_each(required_errors, [&](const auto & error) { summary.missing_state_model_facts[error]++; });
            continue;
        }

        auto transitions = state.apply_fact(fact, route.role, summary);
        summary.processed_hicache_events++;
        summary.processed_events_by_role[hicache_fact_role_name(route.role)]++;
        summary.transition_trace.insert(summary.transition_trace.end(), transitions.begin(), transitions.end());
        summary.state_transition_count = summary.transition_trace.size();
    }

    auto final_transitions = state.finalize(summary);
    summary.transition_trace.insert(summary.transition_trace.end(), final_transitions.begin(), final_transitions.end());
    summary.state_transition_count = summary.transition_trace.size();

    /*
     * validation 默认使用 materialized-only final state；storage-directory-inclusive
     * 视图只用于诊断 L3/backend 已知但尚未 materialize 到 radix tree 的 page。
     */
    const auto final_state = state.derived_state(HiCacheDerivedStateMode::MaterializedOnly);
    const auto inclusive_state = state.derived_state(HiCacheDerivedStateMode::StorageDirectoryInclusive);
    summary.final_state_derivation_mode = hicache_derived_state_mode_name(final_state.mode);
    summary.storage_directory_inclusive_state = inclusive_state;
    summary.active_ref_owner_count = state.active_ref_owner_count();
    summary.radix_split_count = state.radix_split_count();
    summary.radix_split_trace = state.radix_split_trace();
    summary.control_checkpoint_count = state.control_checkpoint_count();
    summary.control_checkpoint_trace = state.control_checkpoint_trace();
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
    return summary;
}

} // namespace markov::trace_graph::modules::hicache::model
