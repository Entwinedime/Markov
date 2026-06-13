/**
 * @file
 * @brief HiCache fact 到 target-state handler 的准入与路由。
 */
#include "trace_graph/modules/hicache/hicache_router.hpp"

#include <unordered_map>

namespace TraceGraph {

namespace {

/** @brief 这些 role 需要完整 token path 或可解析 span 才能构造 target page。 */
bool needs_full_path(HiCacheFactRole role) {
    return role == HiCacheFactRole::RequestBoundMatchAnchor || role == HiCacheFactRole::RequestAdmission || role == HiCacheFactRole::PrefetchDecision;
}

/**
 * @brief 判断 fact 是否携带了足以重建 full path 的 invariant token 信息。
 *
 * 这里不接受 page-level source observation。短于一个 page 的请求无需完整 path，因为
 * 它不会形成可建模 cache page。
 */
bool has_complete_full_path(const HiCacheFact & fact, uint64_t effective_page_size) {
    if (!fact.full_path_tokens.empty()) return true;
    if (fact.full_path_span.valid && fact.full_path_span.begin == fact.full_path_span.end) return true;
    if (fact.full_path_span.valid) return true;
    return effective_page_size == 0 || fact.token_count < effective_page_size;
}

} // namespace

/**
 * @brief HiCache 状态模型的硬输入契约。
 *
 * 只有 model_input 的 atomic invariant_state 才能驱动 target state。这个函数是防止
 * source_actual/timing/oracle/debug 事实越过状态边界的第一道门。
 */
bool is_hicache_state_model_fact(const HiCacheFact & fact) {
    return fact.model_input && fact.fact_class == "invariant_state" && fact.fact_granularity == "atomic";
}

/** @brief 将稳定 event_role 字符串映射到 handler 枚举。 */
HiCacheFactRole parse_hicache_fact_role(const std::string & role) {
    static const std::unordered_map<std::string, HiCacheFactRole> roles = {
        {"request_bound_match_anchor", HiCacheFactRole::RequestBoundMatchAnchor},
        {"request_lifecycle_anchor", HiCacheFactRole::RequestLifecycleAnchor},
        {"request_admission", HiCacheFactRole::RequestAdmission},
        {"prefetch_decision", HiCacheFactRole::PrefetchDecision},
        {"prefetch_check_point", HiCacheFactRole::PrefetchCheckPoint},
    };
    auto it = roles.find(role);
    if (it == roles.end()) return HiCacheFactRole::Unknown;
    return it->second;
}

/** @brief 返回 summary 中使用的稳定 role 名。 */
std::string hicache_fact_role_name(HiCacheFactRole role) {
    switch (role) {
    case HiCacheFactRole::RequestBoundMatchAnchor:
        return "request_bound_match_anchor";
    case HiCacheFactRole::RequestLifecycleAnchor:
        return "request_lifecycle_anchor";
    case HiCacheFactRole::RequestAdmission:
        return "request_admission";
    case HiCacheFactRole::PrefetchDecision:
        return "prefetch_decision";
    case HiCacheFactRole::PrefetchCheckPoint:
        return "prefetch_check_point";
    case HiCacheFactRole::Unknown:
        return "unknown";
    }
    return "unknown";
}

/** @brief 执行输入契约检查和 role 白名单检查。 */
HiCacheFactRoute route_hicache_fact(const HiCacheFact & fact) {
    HiCacheFactRoute route;
    route.model_fact = is_hicache_state_model_fact(fact);
    if (!route.model_fact) return route;
    route.role = parse_hicache_fact_role(fact.role);
    route.known_role = route.role != HiCacheFactRole::Unknown;
    return route;
}

/** @brief 当前 C++ 状态机已经实现的 role 白名单。 */
bool hicache_fact_role_implemented(HiCacheFactRole role) {
    switch (role) {
    case HiCacheFactRole::RequestBoundMatchAnchor:
    case HiCacheFactRole::RequestLifecycleAnchor:
    case HiCacheFactRole::RequestAdmission:
    case HiCacheFactRole::PrefetchDecision:
    case HiCacheFactRole::PrefetchCheckPoint:
        return true;
    case HiCacheFactRole::Unknown:
        return false;
    }
    return false;
}

/**
 * @brief 校验某个 role 的 target-state 必需字段。
 *
 * 返回值直接作为 missing_invariant_facts 的 key 使用，因此字段名保持稳定且具体。
 */
std::vector<std::string> hicache_required_fact_errors(const HiCacheFact & fact, HiCacheFactRole role, uint64_t effective_page_size) {
    std::vector<std::string> errors;
    if (role == HiCacheFactRole::Unknown) {
        errors.push_back("unknown_invariant_role");
        return errors;
    }
    if (fact.cache_scope.empty()) errors.push_back("missing_cache_scope");
    if (fact.seq_no == 0) errors.push_back("missing_seq_no");
    if (role == HiCacheFactRole::RequestBoundMatchAnchor && fact.request_id.empty()) errors.push_back("missing_request_id");
    if (role == HiCacheFactRole::RequestLifecycleAnchor && fact.request_id.empty()) errors.push_back("missing_request_id");
    if (role == HiCacheFactRole::RequestLifecycleAnchor && fact.lifecycle_kind.empty()) errors.push_back("missing_lifecycle_kind");
    if (role == HiCacheFactRole::RequestAdmission && fact.admission_kind.empty()) errors.push_back("missing_admission_kind");
    if ((role == HiCacheFactRole::RequestAdmission || role == HiCacheFactRole::PrefetchDecision || role == HiCacheFactRole::PrefetchCheckPoint) &&
        fact.request_id.empty())
        errors.push_back("missing_request_id");
    if (role == HiCacheFactRole::PrefetchCheckPoint && fact.check_kind.empty()) errors.push_back("missing_check_kind");
    if (needs_full_path(role) && !has_complete_full_path(fact, effective_page_size)) { errors.push_back("token_dictionary_or_full_path_span"); }
    return errors;
}

} // namespace TraceGraph
