/**
 * @file
 * @brief Structured Debug and validation output from the HiCache state model.
 *
 * This file defines the data boundary consumed after model execution. It does not
 * own JSON serialization, file output, or oracle comparison. Release execution
 * neither exposes nor retains these records; Debug diagnostics may serialize them
 * without introducing a dependency from the state machine back to diagnostics.
 */
#pragma once

#ifdef DEBUG
#include "markov/trace_graph/frontend/model_config.hpp"
#include "markov/trace_graph/modules/hicache/policy.hpp"
#include "markov/trace_graph/modules/hicache/radix/token_radix_tree.hpp"
#include "markov/trace_graph/modules/hicache/runtime/async_state.hpp"
#include "markov/trace_graph/modules/hicache/runtime/capacity_index.hpp"
#include "markov/trace_graph/modules/hicache/runtime/ref_ledger.hpp"
#include "markov/trace_graph/modules/hicache/runtime/state_index.hpp"
#include "markov/trace_graph/modules/hicache/runtime/target_control_clock.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#endif

namespace markov::trace_graph::modules::hicache::model {

#ifdef DEBUG
using radix::HiCacheNodeSplitRecord;
using runtime::HiCacheCapacityAuditIssue;
using runtime::HiCacheCapacityMutation;
using runtime::HiCacheCapacityVictimChoice;
using runtime::HiCacheControlBoundary;
using runtime::HiCacheDerivedStateSnapshot;
using runtime::HiCacheOperationLifecycleTransition;
using runtime::HiCacheRefAuditIssue;
using runtime::HiCacheRefMutation;

/**
 * @brief Structured record of one target-derived state mutation.
 *
 * `pages` contains canonical model page IDs, and `source_event_index` identifies
 * the source event in normalized trace order. Digest fields are populated only
 * when `HiCacheConfig::emit_state_digests` is enabled and never affect decisions.
 */
struct HiCacheStateTransition {
    std::string transition_id;
    std::string kind;
    std::string role;
    std::string request_id;
    std::string operation_id;
    std::string event_name;
    std::string cache_scope;
    uint64_t ts = 0;
    size_t source_event_index = 0;
    std::string tier;
    std::vector<std::string> pages;
    std::string before_state_digest;
    std::string after_state_digest;
};

using HiCacheTransitionBuffer = std::vector<HiCacheStateTransition>;
#else
struct HiCacheTransitionBuffer {};
#endif

/**
 * @brief Debug and validation result produced by one HiCache model replay.
 *
 * This is the sole aggregate boundary between replay and diagnostics. Debug builds
 * populate fact counts, policy decisions, asynchronous lifecycle evidence,
 * capacity/reference audits, and final derived state. JSON naming, presentation,
 * and oracle comparison remain outside the model. Release builds do not retain it.
 *
 * Invariants:
 * - page and node values in `*_trace` fields are canonical model identities, not tokens;
 * - `final_state_derivation_mode` names the projection used for final state;
 * - `storage_directory_inclusive_state` supplements, but never replaces, the
 *   materialized-only page sets in `final_state`.
 */
struct HiCacheSummary {
#ifdef DEBUG
    std::string status = "state_model";
    frontend::HiCacheConfig target_config;
    HiCacheResolvedPolicyState resolved_policy;
    uint64_t input_hicache_events = 0;
    uint64_t processed_hicache_events = 0;
    uint64_t state_transition_count = 0;
    uint64_t dirty_eviction_events = 0;
    uint64_t active_ref_owner_count = 0;
    uint64_t radix_split_count = 0;
    uint64_t control_boundary_count = 0;
    uint64_t async_lifecycle_transition_count = 0;
    uint64_t policy_decision_count = 0;
    uint64_t storage_known_page_count = 0;
    uint64_t storage_readable_page_count = 0;
    uint64_t storage_backend_readable_count = 0;
    uint64_t storage_materialized_page_count = 0;
    uint64_t capacity_mutation_count = 0;
    uint64_t capacity_victim_choice_count = 0;
    uint64_t capacity_audit_issue_count = 0;
    uint64_t ref_mutation_count = 0;
    uint64_t ref_audit_issue_count = 0;
    uint64_t skipped_non_state_model_events = 0;
    std::string final_state_derivation_mode;
    std::map<std::string, uint64_t> events_by_role;
    std::map<std::string, uint64_t> processed_events_by_role;
    std::map<std::string, uint64_t> transitions_by_kind;
    std::map<std::string, uint64_t> missing_state_model_facts;
    std::map<std::string, uint64_t> token_resolution_by_status;
    std::map<std::string, uint64_t> token_path_diagnostics;
    std::vector<HiCacheNodeSplitRecord> radix_split_trace;
    std::vector<HiCacheControlBoundary> control_boundary_trace;
    std::vector<HiCacheOperationLifecycleTransition> async_lifecycle_trace;
    std::vector<HiCachePolicyDecisionRecord> policy_decision_trace;
    std::vector<std::string> l1_resident_pages;
    std::vector<std::string> l2_resident_pages;
    std::vector<std::string> l3_resident_pages;
    std::vector<std::string> dirty_pages;
    std::vector<std::string> backuped_pages;
    std::vector<std::string> evicted_pages;
    std::vector<std::string> locked_pages;
    std::vector<std::string> pending_writeback_pages;
    std::vector<std::string> prefetch_planned_pages;
    std::vector<std::string> prefetch_ready_pages;
    std::vector<std::string> prefetch_late_pages;
    std::vector<std::string> prefetch_suppressed_pages;
    std::map<std::string, uint64_t> page_hit_counts;
    HiCacheDerivedStateSnapshot storage_directory_inclusive_state;
    std::vector<HiCacheCapacityMutation> capacity_mutation_trace;
    std::vector<HiCacheCapacityVictimChoice> capacity_victim_choices;
    std::vector<HiCacheCapacityAuditIssue> capacity_audit_issues;
    std::vector<HiCacheRefMutation> ref_mutation_trace;
    std::vector<HiCacheRefAuditIssue> ref_audit_issues;
    std::vector<HiCacheStateTransition> transition_trace;
    std::vector<std::string> warnings;
#endif
};

} // namespace markov::trace_graph::modules::hicache::model
