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
    RoleMapping{ "prefetch_candidate_anchor", HiCacheFactRole::PrefetchCandidateAnchor },
    RoleMapping{        "cache_lookup_input",        HiCacheFactRole::CacheLookupInput },
    RoleMapping{        "cache_extend_input",        HiCacheFactRole::CacheExtendInput },
    RoleMapping{    "cache_lifecycle_commit",    HiCacheFactRole::CacheLifecycleCommit },
};

/**
 * @brief 需要 target page projection 的 role。
 *
 * scalar workload role 必须能从 fact-local span/dictionary 解析出 token path；
 * batch workload role 使用 batch path 数组，不能从 request timeline 猜测补齐。
 */
bool needs_full_path(HiCacheFactRole role) {
    return role == HiCacheFactRole::PrefetchCandidateAnchor || role == HiCacheFactRole::CacheLookupInput || role == HiCacheFactRole::CacheLifecycleCommit;
}

bool has_projectable_path(const HiCacheFact & fact, uint64_t effective_page_size) {
    (void)effective_page_size;
    return hicache_fact_has_resolved_full_path(fact);
}

bool has_projectable_batch_paths(const HiCacheFact & fact) {
    if (fact.batch_paths.empty()) return false;
    return std::ranges::all_of(fact.batch_paths, [](const auto & entry) {
        if (entry.request_id.empty() || !entry.full_path_span.valid) return false;
        if (entry.full_path_span.token_count == 0) return entry.full_path_span.begin == entry.full_path_span.end;
        return static_cast<uint64_t>(entry.full_path_tokens.size()) == entry.full_path_span.token_count;
    });
}

/**
 * @brief 当前 C++ state model 的 class/role 白名单。
 *
 * 这张表是 active 输入合同的一部分。新增可建模 fact 时必须同时更新 probe
 * catalog、Python quality gate 和这里的 router；未知组合不进入 state model。
 */
bool state_model_class_role(const HiCacheFact & fact) {
    if (fact.fact_class != "workload_identity") return false;
    return fact.role == "prefetch_candidate_anchor" || fact.role == "cache_lookup_input" || fact.role == "cache_extend_input"
           || fact.role == "cache_lifecycle_commit";
}

bool completed_model_phase(const HiCacheFact & fact, HiCacheFactRole role) {
    /**
     * @brief state model fact 的可消费 phase 是 active 输入合同的一部分。
     *
     * `cache_extend_input` 必须在 `prepare_for_extend` start phase 读取，其他
     * workload identity fact 使用 completed end phase。
     */
    if (role == HiCacheFactRole::CacheExtendInput) return fact.is_start;
    return fact.is_end;
}

} // namespace router_detail

using router_detail::completed_model_phase;
using router_detail::has_projectable_batch_paths;
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
    /**
     * @brief route 先检查 consumer，再检查 phase 和 class/role。
     *
     * 这样 diagnostics/source_actual 即使 role 名字碰巧相同，也不会误入 target state model。
     */
    HiCacheFactRoute route;
    if (!fact.has_consumer("hicache_state_model")) return route;
    route.role = parse_hicache_fact_role(fact.role);
    route.model_fact = completed_model_phase(fact, route.role);
    if (!route.model_fact) return route;
    route.known_role = route.role != HiCacheFactRole::Unknown && state_model_class_role(fact);
    return route;
}

bool hicache_fact_role_implemented(HiCacheFactRole role) {
    return role == HiCacheFactRole::PrefetchCandidateAnchor || role == HiCacheFactRole::CacheLookupInput || role == HiCacheFactRole::CacheExtendInput
           || role == HiCacheFactRole::CacheLifecycleCommit;
}

std::vector<std::string> hicache_required_fact_errors(const HiCacheFact & fact, HiCacheFactRole role, uint64_t effective_page_size) {
    std::vector<std::string> errors;
    if (role == HiCacheFactRole::Unknown) {
        errors.push_back("unknown_state_model_role");
        return errors;
    }
    if (fact.cache_scope.empty()) errors.push_back("missing_cache_scope");
    if (fact.seq_no == 0) errors.push_back("missing_seq_no");
    if ((role == HiCacheFactRole::PrefetchCandidateAnchor || role == HiCacheFactRole::CacheLookupInput || role == HiCacheFactRole::CacheLifecycleCommit)
        && fact.request_id.empty())
        errors.push_back("missing_request_id");
    if (role == HiCacheFactRole::CacheLifecycleCommit && fact.lifecycle_kind.empty()) errors.push_back("missing_lifecycle_kind");
    if (role == HiCacheFactRole::CacheExtendInput) {
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
    if (needs_full_path(role) && !has_projectable_path(fact, effective_page_size)) errors.push_back("token_dictionary_or_full_path_span");
    return errors;
}

} // namespace markov::trace_graph::modules::hicache
