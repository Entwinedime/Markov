#include "trace_graph/modules/hicache/hicache_router.hpp"

#include <unordered_map>

namespace TraceGraph {

namespace {

bool needs_full_path(HiCacheFactRole role) {
    return role == HiCacheFactRole::RequestTokens || role == HiCacheFactRole::LookupPath || role == HiCacheFactRole::RequestCacheLifecycle ||
           role == HiCacheFactRole::RequestAdmission || role == HiCacheFactRole::InsertPath || role == HiCacheFactRole::PrefetchDecision;
}

bool has_complete_full_path(const HiCacheFact & fact, uint64_t effective_page_size) {
    if (!fact.full_path_tokens.empty()) return true;
    if (fact.full_path_span.valid) return false;
    return effective_page_size == 0 || fact.token_count < effective_page_size;
}

bool has_complete_logical_path(const HiCacheFact & fact, uint64_t effective_page_size) {
    if (!fact.logical_path_tokens.empty()) return true;
    if (fact.logical_path_span.valid) return false;
    return effective_page_size == 0 || fact.token_count < effective_page_size;
}

} // namespace

bool is_hicache_state_model_input(const HiCacheFact & fact) { return fact.fact_class == "invariant_state" && fact.state_model_input; }

HiCacheFactRole parse_hicache_fact_role(const std::string & role) {
    static const std::unordered_map<std::string, HiCacheFactRole> roles = {
        {"request_tokens", HiCacheFactRole::RequestTokens},
        {"lookup_path", HiCacheFactRole::LookupPath},
        {"cache_config_observed", HiCacheFactRole::CacheConfigObserved},
        {"request_cache_lifecycle", HiCacheFactRole::RequestCacheLifecycle},
        {"request_admission", HiCacheFactRole::RequestAdmission},
        {"insert_path", HiCacheFactRole::InsertPath},
        {"prefetch_decision", HiCacheFactRole::PrefetchDecision},
        {"prefetch_check_point", HiCacheFactRole::PrefetchCheckPoint},
        {"maintenance_checkpoint", HiCacheFactRole::MaintenanceCheckpoint},
        {"capacity_request", HiCacheFactRole::CapacityRequest},
        {"lock_scope_delta", HiCacheFactRole::LockScopeDelta},
    };
    auto it = roles.find(role);
    if (it == roles.end()) return HiCacheFactRole::Unknown;
    return it->second;
}

std::string hicache_fact_role_name(HiCacheFactRole role) {
    switch (role) {
    case HiCacheFactRole::RequestTokens:
        return "request_tokens";
    case HiCacheFactRole::LookupPath:
        return "lookup_path";
    case HiCacheFactRole::CacheConfigObserved:
        return "cache_config_observed";
    case HiCacheFactRole::RequestCacheLifecycle:
        return "request_cache_lifecycle";
    case HiCacheFactRole::RequestAdmission:
        return "request_admission";
    case HiCacheFactRole::InsertPath:
        return "insert_path";
    case HiCacheFactRole::PrefetchDecision:
        return "prefetch_decision";
    case HiCacheFactRole::PrefetchCheckPoint:
        return "prefetch_check_point";
    case HiCacheFactRole::MaintenanceCheckpoint:
        return "maintenance_checkpoint";
    case HiCacheFactRole::CapacityRequest:
        return "capacity_request";
    case HiCacheFactRole::LockScopeDelta:
        return "lock_scope_delta";
    case HiCacheFactRole::Unknown:
        return "unknown";
    }
    return "unknown";
}

HiCacheFactRoute route_hicache_fact(const HiCacheFact & fact) {
    HiCacheFactRoute route;
    route.state_model_input = is_hicache_state_model_input(fact);
    if (!route.state_model_input) return route;
    route.role = parse_hicache_fact_role(fact.role);
    route.known_role = route.role != HiCacheFactRole::Unknown;
    return route;
}

bool hicache_fact_role_implemented(HiCacheFactRole role) {
    switch (role) {
    case HiCacheFactRole::RequestTokens:
    case HiCacheFactRole::LookupPath:
    case HiCacheFactRole::CacheConfigObserved:
    case HiCacheFactRole::InsertPath:
    case HiCacheFactRole::PrefetchDecision:
    case HiCacheFactRole::PrefetchCheckPoint:
    case HiCacheFactRole::CapacityRequest:
    case HiCacheFactRole::LockScopeDelta:
        return true;
    case HiCacheFactRole::RequestCacheLifecycle:
    case HiCacheFactRole::RequestAdmission:
    case HiCacheFactRole::MaintenanceCheckpoint:
    case HiCacheFactRole::Unknown:
        return false;
    }
    return false;
}

std::vector<std::string> hicache_required_fact_errors(const HiCacheFact & fact, HiCacheFactRole role, uint64_t effective_page_size) {
    std::vector<std::string> errors;
    if (role == HiCacheFactRole::Unknown) {
        errors.push_back("unknown_invariant_role");
        return errors;
    }
    if (fact.cache_scope.empty()) errors.push_back("missing_cache_scope");
    if (fact.seq_no == 0) errors.push_back("missing_seq_no");
    if (needs_full_path(role) && !has_complete_full_path(fact, effective_page_size)) { errors.push_back("token_dictionary_or_full_path_span"); }
    if (role == HiCacheFactRole::LockScopeDelta && !has_complete_logical_path(fact, effective_page_size)) {
        errors.push_back("token_dictionary_or_logical_path_span");
    }
    return errors;
}

} // namespace TraceGraph
