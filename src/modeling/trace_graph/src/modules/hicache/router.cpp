/**
 * @file
 * @brief HiCache state-model fact 路由与 schema gate。
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
    RoleMapping{ "request_bound_match_anchor", HiCacheFactRole::RequestBoundMatchAnchor },
    RoleMapping{   "request_lifecycle_anchor",  HiCacheFactRole::RequestLifecycleAnchor },
    RoleMapping{          "request_admission",        HiCacheFactRole::RequestAdmission },
    RoleMapping{          "prefetch_decision",        HiCacheFactRole::PrefetchDecision },
    RoleMapping{       "prefetch_check_point",      HiCacheFactRole::PrefetchCheckPoint },
};

/**
 * @brief 需要 target page projection 的 role。
 *
 * checkpoint role 只提供调度边界，不携带 path；其他 state-model role 都必须能从
 * fact-local span/dictionary 解析出 token path，不能从 request timeline 猜测补齐。
 */
bool needs_full_path(HiCacheFactRole role) {
    return role == HiCacheFactRole::RequestBoundMatchAnchor || role == HiCacheFactRole::RequestLifecycleAnchor || role == HiCacheFactRole::RequestAdmission
           || role == HiCacheFactRole::PrefetchDecision;
}

bool has_projectable_path(const HiCacheFact & fact, uint64_t effective_page_size) {
    (void)effective_page_size;
    return hicache_fact_has_resolved_full_path(fact);
}

/**
 * @brief 当前 C++ state model 的 class/role 白名单。
 *
 * 这张表是 active 输入合同的一部分。新增可建模 fact 时必须同时更新 probe
 * catalog、Python quality gate 和这里的 router；未知组合不进入兼容分支。
 */
bool state_model_class_role(const HiCacheFact & fact) {
    if (fact.fact_class == "workload_identity") {
        return fact.role == "request_bound_match_anchor" || fact.role == "request_lifecycle_anchor" || fact.role == "request_admission";
    }
    if (fact.fact_class == "target_policy_input") return fact.role == "prefetch_decision";
    if (fact.fact_class == "runtime_model_checkpoint") return fact.role == "prefetch_check_point";
    return false;
}

bool completed_model_phase(const HiCacheFact & fact, HiCacheFactRole role) {
    /* workload/policy fact 是 duration event，只有 end phase 表示字段完整；
       runtime checkpoint 是 instant event，本身就是完整事实。 */
    if (role == HiCacheFactRole::PrefetchCheckPoint) return fact.phase == "instant";
    if (role == HiCacheFactRole::Unknown && fact.fact_class == "runtime_model_checkpoint" && fact.phase == "instant") return true;
    return fact.is_end;
}

} // namespace router_detail

using router_detail::completed_model_phase;
using router_detail::has_projectable_path;
using router_detail::kRoles;
using router_detail::needs_full_path;
using router_detail::RoleMapping;
using router_detail::state_model_class_role;

HiCacheFactRole parse_hicache_fact_role(const std::string & role) {
    const auto it = std::ranges::find(kRoles, role, &RoleMapping::name);
    return it == kRoles.end() ? HiCacheFactRole::Unknown : it->role;
}

std::string hicache_fact_role_name(HiCacheFactRole role) {
    const auto it = std::ranges::find(kRoles, role, &RoleMapping::role);
    return it == kRoles.end() ? "unknown" : std::string{ it->name };
}

HiCacheFactRoute route_hicache_fact(const HiCacheFact & fact) {
    /* route 先检查 consumer，再检查 phase 和 class/role。这样 diagnostics/source_actual
       即使 role 名字碰巧相同，也不会误入 target state model。 */
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
           || role == HiCacheFactRole::PrefetchDecision || role == HiCacheFactRole::PrefetchCheckPoint;
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
    if (needs_full_path(role) && !has_projectable_path(fact, effective_page_size)) errors.push_back("token_dictionary_or_full_path_span");
    return errors;
}

} // namespace markov::trace_graph::modules::hicache
