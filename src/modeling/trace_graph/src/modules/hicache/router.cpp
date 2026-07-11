/**
 * @file
 * @brief HiCache state-model fact routing and schema gates.
 */
#include "markov/trace_graph/modules/hicache/router.hpp"

#include <algorithm>
#include <array>
#include <ranges>
#include <string_view>

namespace markov::trace_graph::modules::hicache {

namespace router_detail {

struct RoleMapping {
    std::string_view name;
    HiCacheFactRole role = HiCacheFactRole::Unknown;
};

constexpr std::array kRoles = {
    RoleMapping{ "prefetch_candidate_anchor", HiCacheFactRole::PrefetchCandidateAnchor },
    RoleMapping{        "cache_lookup_input",        HiCacheFactRole::CacheLookupInput },
    RoleMapping{        "cache_extend_input",        HiCacheFactRole::CacheExtendInput },
    RoleMapping{    "cache_lifecycle_commit",    HiCacheFactRole::CacheLifecycleCommit },
};

/**
 * @brief Returns whether a scalar role requires fact-local target-page projection.
 *
 * Scalar workload roles must resolve their own span and dictionary. Batch extend uses its
 * explicit path array and cannot infer missing entries from request history.
 */
bool needs_full_path(HiCacheFactRole role) {
    return role == HiCacheFactRole::PrefetchCandidateAnchor || role == HiCacheFactRole::CacheLookupInput || role == HiCacheFactRole::CacheLifecycleCommit;
}

bool has_projectable_path(const HiCacheFact & fact) { return hicache_fact_has_resolved_full_path(fact); }

bool has_projectable_batch_paths(const HiCacheFact & fact) {
    if (fact.batch_paths.empty()) return false;
    return std::ranges::all_of(fact.batch_paths, [](const auto & entry) {
        if (entry.request_id.empty() || !entry.full_path_span.valid) return false;
        if (entry.full_path_span.token_count == 0) return entry.full_path_span.begin == entry.full_path_span.end;
        return static_cast<uint64_t>(entry.full_path_tokens.size()) == entry.full_path_span.token_count;
    });
}

/**
 * @brief Tests the active class/role whitelist.
 *
 * Extending this table requires a coordinated probe-catalog and Python quality-gate change.
 * Unknown combinations remain outside the state model.
 */
bool state_model_class_role(const HiCacheFact & fact) {
    if (fact.fact_class != "workload_identity") return false;
    return fact.role == "prefetch_candidate_anchor" || fact.role == "cache_lookup_input" || fact.role == "cache_extend_input"
           || fact.role == "cache_lifecycle_commit";
}

bool completed_model_phase(const HiCacheFact & fact, HiCacheFactRole role) {
    /**
     * @brief Enforces the consumable phase for each active role.
     *
     * `cache_extend_input` is consumed at `prepare_for_extend` start; the other workload
     * identity roles require completed end facts.
     */
    if (role == HiCacheFactRole::CacheExtendInput) return fact.is_start;
    return fact.is_end;
}

bool requires_request_id(HiCacheFactRole role) {
    return role == HiCacheFactRole::PrefetchCandidateAnchor || role == HiCacheFactRole::CacheLookupInput || role == HiCacheFactRole::CacheLifecycleCommit;
}

void append_common_fact_errors(const HiCacheFact & fact, HiCacheFactRole role, std::vector<std::string> & errors) {
    if (fact.cache_scope.empty()) errors.push_back("missing_cache_scope");
    if (fact.seq_no == 0) errors.push_back("missing_seq_no");
    if (requires_request_id(role) && fact.request_id.empty()) errors.push_back("missing_request_id");
    if (role == HiCacheFactRole::CacheLifecycleCommit && fact.lifecycle_kind.empty()) errors.push_back("missing_lifecycle_kind");
}

void append_extend_shape_errors(const HiCacheFact & fact, std::vector<std::string> & errors) {
    if (fact.batch_kind != "extend") errors.push_back("missing_batch_kind_extend");
    if (!fact.batch_request_ids_array) errors.push_back("request_ids_not_array");
    if (!fact.batch_positions_array) errors.push_back("request_positions_not_array");
    if (!fact.batch_token_dictionaries_array) errors.push_back("token_dictionaries_not_array");
    if (!fact.batch_spans_array) errors.push_back("full_path_spans_not_array");
    if (!fact.batch_token_counts_array) errors.push_back("token_counts_not_array");
    if (fact.batch_paths.empty()) errors.push_back("missing_batch_paths");
    if (fact.batch_request_id_count == 0) errors.push_back("missing_batch_request_ids");
    if (fact.batch_size != fact.batch_paths.size()) errors.push_back("batch_size_mismatch");
    if (fact.batch_position_count != fact.batch_request_id_count) errors.push_back("request_positions_length_mismatch");
    if (fact.batch_token_dictionary_count != fact.batch_request_id_count) errors.push_back("token_dictionaries_length_mismatch");
    if (fact.batch_span_count != fact.batch_request_id_count) errors.push_back("full_path_spans_length_mismatch");
    if (fact.batch_token_count_count != fact.batch_request_id_count) errors.push_back("token_counts_length_mismatch");
    if (!fact.batch_request_ids_unique) errors.push_back("duplicate_batch_request_id");
    if (!fact.batch_positions_cover_indexes) errors.push_back("request_positions_coverage");
    if (!fact.batch_positions_match_request_ids) errors.push_back("request_positions_request_id_mismatch");
    if (!has_projectable_batch_paths(fact)) errors.push_back("batch_token_dictionary_or_full_path_span");
}

} // namespace router_detail

using router_detail::append_common_fact_errors;
using router_detail::append_extend_shape_errors;
using router_detail::completed_model_phase;
using router_detail::has_projectable_batch_paths;
using router_detail::has_projectable_path;
using router_detail::kRoles;
using router_detail::needs_full_path;
using router_detail::RoleMapping;
using router_detail::state_model_class_role;

HiCacheFactRole parse_hicache_fact_role(std::string_view role) {
    const auto it = std::ranges::find(kRoles, role, &RoleMapping::name);
    return it == kRoles.end() ? HiCacheFactRole::Unknown : it->role;
}

std::string hicache_fact_role_name(HiCacheFactRole role) {
    const auto it = std::ranges::find(kRoles, role, &RoleMapping::role);
    return it == kRoles.end() ? "unknown" : std::string{ it->name };
}

HiCacheFactRoute route_hicache_fact(const HiCacheFact & fact) {
    /**
     * @brief Checks declared consumer before phase and class/role eligibility.
     *
     * A diagnostics or source-actual fact therefore cannot enter target replay merely by
     * sharing a role token.
     */
    HiCacheFactRoute route;
    if (!fact.has_consumer("hicache_state_model")) return route;
    route.role = parse_hicache_fact_role(fact.role);
    route.model_fact = completed_model_phase(fact, route.role);
    if (!route.model_fact) return route;
    route.known_role = route.role != HiCacheFactRole::Unknown && state_model_class_role(fact);
    return route;
}

std::vector<std::string> hicache_required_fact_errors(const HiCacheFact & fact, HiCacheFactRole role) {
    std::vector<std::string> errors;
    if (role == HiCacheFactRole::Unknown) {
        errors.push_back("unknown_state_model_role");
        return errors;
    }
    append_common_fact_errors(fact, role, errors);
    if (role == HiCacheFactRole::CacheExtendInput) append_extend_shape_errors(fact, errors);
    if (needs_full_path(role) && !has_projectable_path(fact)) errors.push_back("token_dictionary_or_full_path_span");
    return errors;
}

} // namespace markov::trace_graph::modules::hicache
