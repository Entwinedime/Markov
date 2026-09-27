/**
 * @file
 * @brief HiCache state-model fact routing and input gates.
 */
#include "markov/trace_graph/modules/hicache/router.hpp"

#include <algorithm>
#include <array>
#include <ranges>
#include <string_view>

namespace markov::trace_graph::modules::hicache {

namespace {

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

void append_common_fact_errors(const HiCacheFact & fact, HiCacheFactRole role, std::vector<std::string> & errors) {
    if (fact.cache_scope.empty()) errors.push_back("missing_cache_scope");
    if (fact.seq_no == 0) errors.push_back("missing_seq_no");
    if (needs_full_path(role) && fact.request_id.empty()) errors.push_back("missing_request_id");
    if (role == HiCacheFactRole::CacheLifecycleCommit && fact.lifecycle_kind.empty()) errors.push_back("missing_lifecycle_kind");
}

} // namespace

HiCacheFactRole parse_hicache_fact_role(std::string_view role) {
    const auto it = std::ranges::find(kRoles, role, &RoleMapping::name);
    return it == kRoles.end() ? HiCacheFactRole::Unknown : it->role;
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
    // Extend is an entry boundary; scalar roles require their completed end fact.
    route.model_fact = route.role == HiCacheFactRole::CacheExtendInput ? fact.is_start : fact.is_end;
    if (!route.model_fact) return route;
    route.known_role = route.role != HiCacheFactRole::Unknown && fact.fact_class == "workload_identity";
    return route;
}

std::vector<std::string> hicache_required_fact_errors(const HiCacheFact & fact, HiCacheFactRole role) {
    std::vector<std::string> errors;
    if (role == HiCacheFactRole::Unknown) {
        errors.push_back("unknown_state_model_role");
        return errors;
    }
    append_common_fact_errors(fact, role, errors);
    if (role == HiCacheFactRole::CacheExtendInput) errors.insert(errors.end(), fact.batch_input_errors.begin(), fact.batch_input_errors.end());
    if (needs_full_path(role) && !hicache_token_path_resolved(fact.full_path_span, fact.full_path_tokens))
        errors.push_back("token_dictionary_or_full_path_span");
    return errors;
}

} // namespace markov::trace_graph::modules::hicache
