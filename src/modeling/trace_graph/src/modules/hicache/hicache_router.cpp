/**
 * @file
 * @brief HiCache state-model fact 路由与 schema gate。
 */
#include "trace_graph/modules/hicache/hicache_router.hpp"

#include <algorithm>
#include <array>
#include <ranges>
#include <string_view>

namespace TraceGraph {

namespace {

struct RoleMapping {
    std::string_view name;
    HiCacheFactRole role = HiCacheFactRole::Unknown;
};

constexpr std::array kRoles = {
    RoleMapping{ "request_bound_match_anchor", HiCacheFactRole::RequestBoundMatchAnchor },
    RoleMapping{   "request_lifecycle_anchor",  HiCacheFactRole::RequestLifecycleAnchor },
    RoleMapping{          "request_admission",        HiCacheFactRole::RequestAdmission },
    RoleMapping{          "prefetch_decision",        HiCacheFactRole::PrefetchDecision },
    RoleMapping{       "prefetch_check_point",      HiCacheFactRole::PrefetchCheckPoint },
    RoleMapping{ "storage_control_drain_boundary", HiCacheFactRole::StorageControlDrainBoundary },
    RoleMapping{   "storage_backend_readable",  HiCacheFactRole::StorageBackendReadable },
};

bool needs_full_path(HiCacheFactRole role) {
    return role == HiCacheFactRole::RequestBoundMatchAnchor || role == HiCacheFactRole::RequestLifecycleAnchor || role == HiCacheFactRole::RequestAdmission
           || role == HiCacheFactRole::PrefetchDecision;
}

bool has_projectable_path(const HiCacheFact & fact, uint64_t effective_page_size) {
    (void)effective_page_size;
    return hicache_fact_has_resolved_full_path(fact);
}

bool state_model_class_role(const HiCacheFact & fact) {
    if (fact.fact_class == "workload_identity") {
        return fact.role == "request_bound_match_anchor" || fact.role == "request_lifecycle_anchor" || fact.role == "request_admission";
    }
    if (fact.fact_class == "target_policy_input") return fact.role == "prefetch_decision";
    if (fact.fact_class == "runtime_model_checkpoint") {
        return fact.role == "prefetch_check_point" || fact.role == "storage_control_drain_boundary";
    }
    return false;
}

bool completed_model_phase(const HiCacheFact & fact, HiCacheFactRole role) {
    if (role == HiCacheFactRole::PrefetchCheckPoint || role == HiCacheFactRole::StorageControlDrainBoundary) return fact.phase == "instant";
    if (role == HiCacheFactRole::Unknown && fact.fact_class == "runtime_model_checkpoint" && fact.phase == "instant") return true;
    return fact.is_end;
}

} // namespace

HiCacheFactRole parse_hicache_fact_role(const std::string & role) {
    const auto it = std::ranges::find(kRoles, role, &RoleMapping::name);
    return it == kRoles.end() ? HiCacheFactRole::Unknown : it->role;
}

std::string hicache_fact_role_name(HiCacheFactRole role) {
    const auto it = std::ranges::find(kRoles, role, &RoleMapping::role);
    return it == kRoles.end() ? "unknown" : std::string{ it->name };
}

HiCacheFactRoute route_hicache_fact(const HiCacheFact & fact) {
    HiCacheFactRoute route;
    if (!fact.has_consumer("hicache_state_model")) return route;
    route.role = parse_hicache_fact_role(fact.role);
    route.model_fact = completed_model_phase(fact, route.role);
    if (!route.model_fact) return route;
    route.known_role = route.role != HiCacheFactRole::Unknown && state_model_class_role(fact);
    return route;
}

bool hicache_fact_role_implemented(HiCacheFactRole role) {
    return role == HiCacheFactRole::RequestBoundMatchAnchor || role == HiCacheFactRole::RequestLifecycleAnchor || role == HiCacheFactRole::RequestAdmission
           || role == HiCacheFactRole::PrefetchDecision || role == HiCacheFactRole::PrefetchCheckPoint
           || role == HiCacheFactRole::StorageControlDrainBoundary || role == HiCacheFactRole::StorageBackendReadable;
}

std::vector<std::string> hicache_required_fact_errors(const HiCacheFact & fact, HiCacheFactRole role, uint64_t effective_page_size) {
    std::vector<std::string> errors;
    if (role == HiCacheFactRole::Unknown) {
        errors.push_back("unknown_state_model_role");
        return errors;
    }
    if (fact.cache_scope.empty()) errors.push_back("missing_cache_scope");
    if (fact.seq_no == 0) errors.push_back("missing_seq_no");
    if ((role == HiCacheFactRole::RequestBoundMatchAnchor || role == HiCacheFactRole::RequestLifecycleAnchor || role == HiCacheFactRole::RequestAdmission
         || role == HiCacheFactRole::PrefetchDecision || role == HiCacheFactRole::PrefetchCheckPoint)
        && fact.request_id.empty())
        errors.push_back("missing_request_id");
    if (role == HiCacheFactRole::RequestLifecycleAnchor && fact.lifecycle_kind.empty()) errors.push_back("missing_lifecycle_kind");
    if (role == HiCacheFactRole::RequestAdmission && fact.admission_kind.empty()) errors.push_back("missing_admission_kind");
    if (role == HiCacheFactRole::PrefetchCheckPoint && fact.check_kind.empty()) errors.push_back("missing_check_kind");
    if (role == HiCacheFactRole::StorageControlDrainBoundary) {
        if (fact.source_page_size == 0) errors.push_back("missing_source_page_size");
        if (fact.check_kind.empty()) errors.push_back("missing_check_kind");
        else if (fact.check_kind != "storage_control_drain") errors.push_back("invalid_storage_control_check_kind");
    }
    if (role == HiCacheFactRole::StorageBackendReadable && fact.storage_page_hashes.empty()) errors.push_back("missing_storage_page_hashes");
    if (needs_full_path(role) && !has_projectable_path(fact, effective_page_size)) errors.push_back("token_dictionary_or_full_path_span");
    return errors;
}

} // namespace TraceGraph
