/**
 * @file
 * @brief HiCache target-state 状态模型的确定性实现。
 *
 * 本文件承载 HiCacheModule 使用的实际状态机。模型有意保持 target-derived：
 * trace event 只提供 atomic invariant fact，residency、cleanup、request
 * protection 和 prefetch visibility 都从目标配置与 modeled radix topology 推导。
 */
#include "trace_graph/modules/hicache/hicache_model.hpp"

#include "trace_graph/core/dag_graph.hpp"
#include "trace_graph/modules/hicache/hicache_router.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <map>
#include <ranges>
#include <sstream>
#include <utility>

namespace TraceGraph {

namespace {

/** @brief SGLang HiRadixCache 默认 prefetch threshold，单位是 token。 */
constexpr uint64_t kSglangDefaultPrefetchThresholdTokens = 256;

std::string lower(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::vector<std::string> sorted_vector(const std::set<std::string> & values) { return { values.begin(), values.end() }; }

std::set<std::string> non_empty_page_set(const std::vector<std::string> & pages) {
    std::set<std::string> result;
    std::ranges::copy(pages | std::views::filter([](const auto & page) { return !page.empty(); }), std::inserter(result, result.end()));
    return result;
}

std::string join_scoped_page_sets(const std::unordered_map<std::string, std::set<std::string>> & pages_by_scope) {
    std::map<std::string, std::set<std::string>> ordered(pages_by_scope.begin(), pages_by_scope.end());
    std::ostringstream os;
    bool first_scope = true;
    std::ranges::for_each(ordered, [&](const auto & item) {
        const auto & [scope, pages] = item;
        if (!first_scope) os << "|";
        first_scope = false;
        os << scope << "=";
        bool first_page = true;
        std::ranges::for_each(pages, [&](const auto & page) {
            if (!first_page) os << ",";
            first_page = false;
            os << page;
        });
    });
    return os.str();
}

template <typename T> std::vector<T> slice_vector(const std::vector<T> & values, size_t begin, size_t end) {
    if (begin >= values.size() || begin >= end) return {};
    const auto bounded_end = std::min(end, values.size());
    auto view = values | std::views::drop(begin) | std::views::take(bounded_end - begin);
    return { view.begin(), view.end() };
}

template <typename T> std::vector<T> suffix_vector(const std::vector<T> & values, size_t begin) { return slice_vector(values, begin, values.size()); }

std::vector<std::string> slice_pages(const std::vector<std::string> & pages, size_t begin, size_t end) { return slice_vector(pages, begin, end); }

std::vector<std::string> suffix_pages(const std::vector<std::string> & pages, size_t begin) { return suffix_vector(pages, begin); }

HiCacheTokenPath slice_token_path(const HiCacheTokenPath & tokens, size_t begin, size_t end) { return slice_vector(tokens, begin, end); }

size_t first_lru_order_index(const std::vector<std::string> & group, const std::vector<std::string> & order) {
    auto best = order.size();
    std::ranges::for_each(group, [&](const auto & page) {
        const auto order_it = std::ranges::find(order, page);
        if (order_it != order.end()) best = std::min(best, static_cast<size_t>(std::distance(order.begin(), order_it)));
    });
    return best;
}

std::vector<std::string> earliest_lru_group(const std::vector<std::vector<std::string>> & candidate_groups, const std::vector<std::string> & order) {
    const auto best_group = std::ranges::min_element(candidate_groups, {}, [&](const auto & group) { return first_lru_order_index(group, order); });
    if (best_group == candidate_groups.end()) return {};
    return first_lru_order_index(*best_group, order) == order.size() ? std::vector<std::string>{} : *best_group;
}

size_t page_aligned_token_count(size_t token_count, uint64_t page_size) {
    if (token_count == 0 || page_size == 0) return 0;
    const auto page = static_cast<size_t>(page_size);
    return token_count / page * page;
}

uint64_t ceil_div(uint64_t value, uint64_t divisor) {
    if (value == 0 || divisor == 0) return 0;
    return (value + divisor - 1) / divisor;
}

} // namespace

std::string HiCacheState::digest() const { return state_index_.digest() + ";pending_writeback=" + join_scoped_page_sets(pending_writeback_pages_by_scope_); }

HiCacheState::HiCacheState(HiCacheConfig config) : config_(std::move(config)), target_pager_(config_) {}

/**
 * @brief 将 per-scope writeback queue 展平成 summary 暴露的 page set。
 *
 * 内部 map 保持 scope 维度，避免 capacity enforcement 和 cleanup 意外混用独立
 * cache scope。summary 有意只暴露 page id，因为 final-state consumer 比较的是
 * page state。
 */
std::set<std::string> HiCacheState::pending_writeback() const {
    std::set<std::string> pages;
    std::ranges::for_each(pending_writeback_pages_by_scope_ | std::views::values,
                          [&](const auto & pending_pages) { pages.insert(pending_pages.begin(), pending_pages.end()); });
    return pages;
}

uint64_t HiCacheState::page_size_for_fact(const HiCacheFact & fact) const { return target_pager_.page_size_for_fact(fact); }

std::vector<std::string> HiCacheState::pages_for_tokens(const HiCacheFact & fact, const HiCacheTokenPath & tokens) const {
    return target_pager_.pages_for_tokens(fact, tokens);
}

uint64_t HiCacheState::full_path_token_count(const HiCacheFact & fact) const {
    if (fact.full_path_span.valid) {
        if (fact.full_path_span.token_count > 0) return fact.full_path_span.token_count;
        if (fact.full_path_span.end >= fact.full_path_span.begin) return fact.full_path_span.end - fact.full_path_span.begin;
    }
    if (fact.token_count > 0) return fact.token_count;
    return static_cast<uint64_t>(fact.full_path_tokens.size());
}

uint64_t HiCacheState::projected_aligned_token_count(const HiCacheFact & fact) const {
    const auto page_size = page_size_for_fact(fact);
    if (page_size == 0) return 0;
    const auto token_count = full_path_token_count(fact);
    return token_count / page_size * page_size;
}

bool HiCacheState::needs_projected_pages(const HiCacheFact & fact) const { return projected_aligned_token_count(fact) > 0; }

/**
 * @brief 为 fact 解析 target page-aligned token path。
 *
 * 直接携带的 full_path_tokens 优先。如果 fact 只携带 span，parser 可能已经通过
 * token dictionary 将其解析出来；否则模型退回到之前 atomic invariant fact 记录
 * 的 request-local tokens。返回空 path 表示 target projection 不可构造，而不是
 * 允许导入 source 观测到的 page state。
 */
HiCacheTokenPath HiCacheState::projected_full_path_tokens_for_fact(const HiCacheFact & fact) const {
    const auto desired = projected_aligned_token_count(fact);
    if (desired == 0) return {};
    const auto desired_size = static_cast<size_t>(desired);

    auto slice_if_sufficient = [&](const HiCacheTokenPath & tokens) -> HiCacheTokenPath {
        if (tokens.size() < desired_size) return {};
        return slice_token_path(tokens, 0, desired_size);
    };

    if (!fact.full_path_tokens.empty()) {
        auto tokens = slice_if_sufficient(fact.full_path_tokens);
        if (!tokens.empty()) return tokens;
    }

    auto request_tokens = token_store_.request_tokens(fact);
    if (!request_tokens.empty()) {
        auto tokens = slice_if_sufficient(request_tokens);
        if (!tokens.empty()) return tokens;
    }
    return {};
}

void HiCacheState::note_missing_projection_if_needed(const HiCacheFact & fact, HiCacheSummary & summary) const {
    if (needs_projected_pages(fact)) summary.missing_invariant_facts["target_page_projection_tokens"]++;
}

std::set<std::string> * HiCacheState::tier_set(const std::string & tier) { return state_index_.tier_set(tier); }

const std::set<std::string> * HiCacheState::tier_set(const std::string & tier) const { return state_index_.tier_set(tier); }

std::vector<std::string> * HiCacheState::touch_order_for_tier(const std::string & tier) { return state_index_.touch_order_for_tier(tier); }

uint64_t HiCacheState::capacity_for_tier(const std::string & tier) const {
    if (tier == "L1") return config_.l1_capacity_pages;
    if (tier == "L2") return config_.l2_capacity_pages;
    return 0;
}

uint64_t HiCacheState::target_write_through_threshold() const {
    if (config_.write_through_threshold > 0) return config_.write_through_threshold;
    if (config_.write_policy == "write_through") return 1;
    if (config_.write_policy == "write_through_selective") return 2;
    return 0;
}

bool HiCacheState::target_write_count_enabled() const { return config_.write_policy == "write_through" || config_.write_policy == "write_through_selective"; }

bool HiCacheState::target_write_back_enabled() const { return config_.write_policy == "write_back"; }

/**
 * @brief 返回配置值或 SGLang 默认值推导出的 prefetch threshold，单位是 page。
 *
 * 默认值来自 SGLang 的 256-token threshold，并向上取整到完整 target page。
 * page size 为 0 时无法安全构造 target page projection，因此不推导 threshold。
 */
uint64_t HiCacheState::target_prefetch_threshold_pages() const {
    if (config_.prefetch_threshold_pages > 0) return config_.prefetch_threshold_pages;
    const auto page_size = config_.page_size;
    if (page_size == 0) return 0;
    return std::max<uint64_t>(1, ceil_div(kSglangDefaultPrefetchThresholdTokens, page_size));
}

/**
 * @brief 返回 in-flight best-effort prefetch request 的 target cap。
 *
 * SGLang 用 HiCache host-only pool 的 80% 限制 prefetch pressure。模型优先使用
 * trace/config 采到的显式 cap；没有显式 override 时，用
 * (l2_capacity_pages - l1_capacity_pages) 的五分之四投影源码语义。返回 0 表示
 * 没有可用 prefetch capacity，不能解释成无限制。
 */
uint64_t HiCacheState::target_prefetch_capacity_limit_pages() const {
    if (config_.prefetch_capacity_limit_pages > 0) return config_.prefetch_capacity_limit_pages;
    if (config_.l2_capacity_pages <= config_.l1_capacity_pages) return 0;
    const auto host_only_pages = config_.l2_capacity_pages - config_.l1_capacity_pages;
    return host_only_pages * 4 / 5;
}

std::string HiCacheState::scoped_request_key(const HiCacheFact & fact) const { return token_store_.scoped_request_key(fact); }

std::string HiCacheState::normalized_scope(const HiCacheFact & fact) const { return fact.cache_scope.empty() ? std::string("-1") : fact.cache_scope; }

bool HiCacheState::page_in_scope(const std::string & page, const std::string & scope) const {
    const auto separator = page.find('|');
    if (separator == std::string::npos) return true;
    return page.compare(0, separator, scope) == 0;
}

uint64_t HiCacheState::tier_size_for_scope(const std::string & tier, const std::string & scope) const {
    const auto * pages = tier_set(tier);
    if (!pages) return 0;
    return static_cast<uint64_t>(std::ranges::count_if(*pages, [&](const auto & page) { return page_in_scope(page, scope); }));
}

uint64_t HiCacheState::active_prefetch_requested_pages_for_scope(const std::string & scope) const {
    uint64_t pages = 0;
    for (const auto & work : async_state_.prefetch_by_request | std::views::values) {
        if (work.cache_scope == scope && (work.state == PrefetchWorkState::Pending || work.state == PrefetchWorkState::Ready))
            pages += work.requested_host_pages;
    }
    return pages;
}

uint64_t HiCacheState::reserved_prefetch_pages_for_scope(const std::string & scope) const {
    uint64_t pages = 0;
    for (const auto & work : async_state_.prefetch_by_request | std::views::values) {
        if (work.cache_scope == scope && (work.state == PrefetchWorkState::Pending || work.state == PrefetchWorkState::Ready))
            pages += work.reserved_host_pages;
    }
    return pages;
}

uint64_t HiCacheState::host_pool_occupied_pages_for_scope(const std::string & scope) const {
    return tier_size_for_scope("L2", scope) + reserved_prefetch_pages_for_scope(scope);
}

uint64_t HiCacheState::host_pool_available_pages_for_scope(const std::string & scope) const {
    if (config_.l2_capacity_pages == 0) return 0;
    const auto occupied = host_pool_occupied_pages_for_scope(scope);
    return occupied >= config_.l2_capacity_pages ? 0 : config_.l2_capacity_pages - occupied;
}

HiCacheTokenRadixTree & HiCacheState::device_radix_tree_for_fact(const HiCacheFact & fact) { return device_state_.radix_trees[normalized_scope(fact)]; }

HiCacheTokenRadixTree & HiCacheState::host_radix_tree_for_fact(const HiCacheFact & fact) { return host_state_.radix_trees[normalized_scope(fact)]; }

/**
 * @brief 返回 target lookup 当前可见的连续前缀。
 *
 * radix tree 只描述 modeled topology；page 还必须在请求的 tier set 中 resident，
 * 才能算作 visible。因此遇到第一个 residency gap 时必须停止。
 */
std::vector<std::string> HiCacheState::contiguous_resident_prefix(const std::vector<std::string> & full_pages, size_t max_pages, bool include_device,
                                                                  bool include_host, bool include_storage) const {
    std::vector<std::string> pages;
    pages.reserve(std::min(max_pages, full_pages.size()));
    for (const auto & page : full_pages | std::views::take(max_pages)) {
        const bool resident = (include_device && state_index_.l1().contains(page)) || (include_host && state_index_.l2().contains(page))
                              || (include_storage && state_index_.l3().contains(page));
        if (!resident) break;
        pages.push_back(page);
    }
    return pages;
}

/**
 * @brief 在 modeled device 与 host 视图上匹配 request path。
 *
 * device、host-visible 和 storage-known match 有意分开计算。source trace 可能
 * 报告更长的 lookup，但 target state 只能看见同时存在于 modeled radix topology
 * 和对应 residency set 中的前缀。
 */
HiCacheState::RequestLookupMatch HiCacheState::match_request_lookup_path(const HiCacheFact & fact, const std::vector<std::string> & full_pages) {
    RequestLookupMatch result{};
    auto & device_tree = device_radix_tree_for_fact(fact);
    auto & host_tree = host_radix_tree_for_fact(fact);
    const auto device_match = device_tree.match_page_path(full_pages);
    const auto host_match = host_tree.match_page_path(full_pages);

    result.device_chain_groups = device_match.ancestor_page_groups;
    result.host_chain_groups = host_match.ancestor_page_groups;
    result.device_pages = contiguous_resident_prefix(full_pages, device_match.matched_pages.size(), true, false, false);
    result.host_visible_pages = contiguous_resident_prefix(full_pages, host_match.matched_pages.size(), false, true, true);

    const auto max_visible = std::max(result.device_pages.size(), result.host_visible_pages.size());
    result.visible_pages = contiguous_resident_prefix(full_pages, max_visible, true, true, true);
    return result;
}

void HiCacheState::insert_host_path_topology(const HiCacheFact & fact, const std::vector<std::string> & pages) {
    if (pages.empty()) return;
    (void)host_radix_tree_for_fact(fact).insert_page_path(pages);
}

void HiCacheState::insert_host_page_topology_from_device(const HiCacheFact & fact, const std::string & page) {
    if (page.empty() || host_radix_tree_for_fact(fact).contains_page(page)) return;
    auto path = device_radix_tree_for_fact(fact).page_path_for_page(page);
    if (path.empty()) path = { page };
    insert_host_path_topology(fact, path);
}

/**
 * @brief 应用一个已经完成路由的 atomic invariant fact。
 *
 * role 路由和必需字段校验在 HiCacheStateModel::run() 中完成。这里的 switch 只是
 * 纯 role dispatch 层，避免 non-model fact 在状态机内长出隐式兼容行为。
 */
std::vector<HiCacheStateTransition> HiCacheState::apply_fact(const HiCacheFact & fact, HiCacheFactRole role, HiCacheSummary & summary) {
    std::vector<HiCacheStateTransition> transitions;
    switch (role) {
    case HiCacheFactRole::RequestBoundMatchAnchor:
        apply_request_tokens(fact);
        apply_request_bound_match_anchor(fact, summary, transitions);
        break;
    case HiCacheFactRole::RequestLifecycleAnchor:
        apply_request_lifecycle_anchor(fact, summary, transitions);
        break;
    case HiCacheFactRole::RequestAdmission:
        apply_request_admission(fact, summary, transitions);
        break;
    case HiCacheFactRole::PrefetchDecision:
        apply_prefetch_decision(fact, summary, transitions);
        break;
    case HiCacheFactRole::PrefetchCheckPoint:
        apply_prefetch_check_point(fact, summary, transitions);
        break;
    case HiCacheFactRole::Unknown:
        break;
    }
    return transitions;
}

/**
 * @brief 所有 fact 读完后收束剩余的 modeled async state。
 *
 * 从未到达 target checkpoint 的 prefetch work 会在这里被 suppressed。finalization
 * 只关闭模型内还持有的 reservation / anchor ref，不导入任何 source final-state
 * page。
 */
std::vector<HiCacheStateTransition> HiCacheState::finalize(HiCacheSummary & summary) {
    std::vector<HiCacheStateTransition> transitions;
    HiCacheFact fact{};
    fact.role = "prefetch_finalize";
    fact.event_name = "hicache_prefetch_finalize";
    for (auto it = async_state_.prefetch_by_request.begin(); it != async_state_.prefetch_by_request.end();) {
        auto & work = it->second;
        if (prefetch_work_open(work)) { suppress_prefetch_work(fact, summary, transitions, work); }
        else { release_prefetch_work_resources(fact, summary, transitions, work); }
        it = async_state_.prefetch_by_request.erase(it);
    }
    return transitions;
}

/**
 * @brief 保存 request-bound fact 提供的 token context。
 *
 * request-bound match anchor 是最早可靠获知 request full path 的位置。之后
 * admission、lifecycle 和 prefetch fact 可以通过 token store 投影同一组 target
 * pages，而不需要把 observed page state 当作输入。
 */
void HiCacheState::apply_request_tokens(const HiCacheFact & fact) {
    token_store_.set_request_tokens(fact, fact.full_path_tokens);
    token_store_.observe_request_bound_tokens(fact, fact.full_path_tokens);
}

/**
 * @brief 确保 fact 具有足够 token context 来完成 target page projection。
 *
 * 如果 projection 输入缺失，只在 summary 中记录缺失项；不会退回到
 * source_actual residency 或 diagnostic state injection。
 */
void HiCacheState::apply_request_context(const HiCacheFact & fact, HiCacheSummary & summary) {
    if (!fact.full_path_tokens.empty()) {
        token_store_.set_request_tokens(fact, fact.full_path_tokens);
        return;
    }
    auto tokens = projected_full_path_tokens_for_fact(fact);
    if (!tokens.empty()) {
        token_store_.set_request_tokens(fact, tokens);
        return;
    }
    note_missing_projection_if_needed(fact, summary);
}

/**
 * @brief 在保留 radix ancestor 分组语义的前提下生成去重 page list。
 *
 * 输入按 radix node 分组，但 lock/ref accounting 需要扁平集合。这里去重时保留
 * first-seen 顺序，使 transition output 保持确定性。
 */
std::vector<std::string> HiCacheState::flatten_page_groups(const std::vector<std::vector<std::string>> & groups) const {
    std::vector<std::string> pages;
    std::set<std::string> seen;
    std::ranges::for_each(groups, [&](const auto & group) {
        std::ranges::for_each(group, [&](const auto & page) {
            if (!page.empty() && seen.insert(page).second) pages.push_back(page);
        });
    });
    return pages;
}

/**
 * @brief 更新 request 的最近一次 target path 快照。
 *
 * request_state 是后续 admission reservation、device lock、host ref 和 unfinished
 * lifecycle 复用的唯一来源。这里保存的是 target lookup/admission 后的 modeled
 * 结果，而不是 source trace 观察到的命中结果。
 */
void HiCacheState::update_request_path_state(const HiCacheFact & fact, const std::vector<std::string> & full_pages,
                                             const std::vector<std::string> & matched_pages, const std::vector<std::vector<std::string>> & device_chain_groups,
                                             const std::vector<std::vector<std::string>> & host_chain_groups) {
    const auto key = scoped_request_key(fact);
    if (key.empty()) return;
    auto & state = request_states_[key];
    state.full_pages = full_pages;
    state.matched_device_prefix_pages = matched_pages;
    state.last_device_chain_groups = device_chain_groups;
    state.last_host_chain_groups = host_chain_groups;
}

/**
 * @brief 统计指定 scope 中仍被 admission reservation 占用的 device page。
 *
 * reservation 表示 request 即将扩展但尚未完成 insert 的 target-side 压力，用于在
 * L1 capacity enforcement 前预留空间。
 */
uint64_t HiCacheState::active_device_reservation_pages_for_scope(const std::string & scope) const {
    uint64_t pages = 0;
    const auto prefix = scope + ":";
    for (const auto & [request_key, state] : request_states_) {
        if (request_key.starts_with(prefix)) pages += state.device_reservation_pages;
    }
    return pages;
}

/**
 * @brief 估算一次 admission 需要为 extend 预留的 token 数。
 *
 * 该估算只基于 target-visible device prefix 和 full_path token count。多加一个
 * page_size 是为了覆盖 SGLang decode/extend 边界上的下一页压力。
 */
uint64_t HiCacheState::estimate_admission_requested_tokens(const HiCacheFact & fact, const RequestExecutionState & request_state) const {
    const auto page_size = page_size_for_fact(fact);
    if (page_size == 0) return 0;
    const auto full_tokens = full_path_token_count(fact);
    const auto matched_tokens = static_cast<uint64_t>(request_state.matched_device_prefix_pages.size()) * page_size;
    const auto extend_tokens = full_tokens > matched_tokens ? full_tokens - matched_tokens : 0;
    return extend_tokens + page_size;
}

/**
 * @brief 将 admission token 压力转换为 page 压力。
 *
 * best_effort 模式按未命中的 full page 数预留；其他模式按 token 估算值向上取整。
 * 两者都只依赖 target projection 与 modeled match prefix。
 */
uint64_t HiCacheState::estimate_admission_requested_pages(const HiCacheFact & fact, const RequestExecutionState & request_state) const {
    if (config_.prefetch_policy == "best_effort") {
        const auto full_pages = static_cast<uint64_t>(request_state.full_pages.size());
        const auto matched_pages = static_cast<uint64_t>(request_state.matched_device_prefix_pages.size());
        return full_pages > matched_pages ? full_pages - matched_pages : 0;
    }
    const auto page_size = page_size_for_fact(fact);
    if (page_size == 0) return 0;
    return ceil_div(estimate_admission_requested_tokens(fact, request_state), page_size);
}

/** @brief 清除 request 的 device reservation，但保留 path 快照和 lock/ref 状态。 */
void HiCacheState::clear_device_reservation(const std::string & request_key) {
    auto it = request_states_.find(request_key);
    if (it != request_states_.end()) it->second.device_reservation_pages = 0;
}

/**
 * @brief 为 request 获取 device page lock。
 *
 * lock 是 target eviction protection，不是 trace 中旧锁增量事件的 replay。
 * page 级计数允许多个 request 共享同一 ancestor page，最后一个 request 释放后才
 * 清除 locked 标记。
 */
void HiCacheState::acquire_device_request_lock(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                               const std::string & request_key, const std::vector<std::string> & pages) {
    if (request_key.empty()) return;
    const auto desired = non_empty_page_set(pages);
    if (desired.empty()) return;
    auto & held = device_state_.request_lock_pages[request_key];
    for (const auto & page : desired) {
        if (!held.insert(page).second) continue;
        const auto before = device_state_.lock_count_by_page[page]++;
        if (before == 0) mark_locked(fact, summary, transitions, page);
    }
}

/**
 * @brief 释放 request 持有的全部 device page lock。
 *
 * release 只影响该 request 已登记的 lock 集合，不会根据 source 事件里的额外字段
 * 修正 target locked state。
 */
void HiCacheState::release_device_request_lock(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                               const std::string & request_key) {
    auto it = device_state_.request_lock_pages.find(request_key);
    if (it == device_state_.request_lock_pages.end()) return;
    for (const auto & page : it->second) {
        auto count_it = device_state_.lock_count_by_page.find(page);
        if (count_it == device_state_.lock_count_by_page.end() || count_it->second == 0) continue;
        count_it->second--;
        if (count_it->second == 0) {
            device_state_.lock_count_by_page.erase(count_it);
            clear_locked(fact, summary, transitions, page);
        }
    }
    device_state_.request_lock_pages.erase(it);
}

/** @brief 用新的 ancestor page 集合替换 request 的 device lock。 */
void HiCacheState::replace_device_request_lock(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                               const std::string & request_key, const std::vector<std::string> & pages) {
    release_device_request_lock(fact, summary, transitions, request_key);
    acquire_device_request_lock(fact, summary, transitions, request_key, pages);
}

/**
 * @brief 为 request 获取 host page ref。
 *
 * host ref 保护 L2/backuped page 不被 host cleanup 提前回收。它只维护内部计数，不
 * 直接发 transition，因为 final state 没有单独的 host-ref 集合。
 */
void HiCacheState::acquire_host_request_ref(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                            const std::string & request_key, const std::vector<std::string> & pages) {
    (void)summary;
    (void)transitions;
    if (request_key.empty()) return;
    const auto desired = non_empty_page_set(pages);
    if (desired.empty()) return;
    auto & held = host_state_.request_lock_pages[request_key];
    for (const auto & page : desired) {
        if (!held.insert(page).second) continue;
        host_state_.ref_count_by_page[page]++;
    }
    (void)fact;
}

/** @brief 释放 request 持有的 host ref，并在计数归零时移除保护。 */
void HiCacheState::release_host_request_ref(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                            const std::string & request_key) {
    (void)fact;
    (void)summary;
    (void)transitions;
    auto it = host_state_.request_lock_pages.find(request_key);
    if (it == host_state_.request_lock_pages.end()) return;
    for (const auto & page : it->second) {
        auto count_it = host_state_.ref_count_by_page.find(page);
        if (count_it == host_state_.ref_count_by_page.end() || count_it->second == 0) continue;
        count_it->second--;
        if (count_it->second == 0) host_state_.ref_count_by_page.erase(count_it);
    }
    host_state_.request_lock_pages.erase(it);
}

/** @brief 用新的 ancestor page 集合替换 request 的 host ref。 */
void HiCacheState::replace_host_request_ref(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                            const std::string & request_key, const std::vector<std::string> & pages) {
    release_host_request_ref(fact, summary, transitions, request_key);
    acquire_host_request_ref(fact, summary, transitions, request_key, pages);
}

/**
 * @brief 根据 target admission pressure 触发 L1 cleanup。
 *
 * cleanup budget 来自 modeled request pressure，而不是“当前超容量多少”。这样可以
 * 在真正 insert 前为即将到来的 device pages 腾出空间。
 */
void HiCacheState::apply_target_device_pressure(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                                uint64_t requested_pages) {
    if (requested_pages == 0 || config_.l1_capacity_pages == 0) return;
    const auto scope = normalized_scope(fact);
    const auto occupied = tier_size_for_scope("L1", scope) + active_device_reservation_pages_for_scope(scope);
    if (occupied + requested_pages <= config_.l1_capacity_pages) return;
    evict_lru_pages(fact, summary, transitions, "L1", requested_pages);
}

/**
 * @brief 为 prefetch request 预留 host page budget。
 *
 * 当 host pool 不足时，对齐 SGLang HiRadixCache::prefetch_from_storage()：allocation
 * failure 调用的是 evict_host(prefetch_length)，也就是按请求长度 cleanup，而不是按
 * deficit cleanup。cleanup 后仍无法完整容纳时，再按实际 available 截断 reservation，
 * 并重新检查 threshold。
 */
uint64_t HiCacheState::reserve_host_pages_for_prefetch(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                                       const std::string & scope, uint64_t requested_pages) {
    if (requested_pages == 0) return 0;
    if (config_.l2_capacity_pages == 0) return requested_pages;

    if (requested_pages > host_pool_available_pages_for_scope(scope)) { evict_host_pages_for_scope(fact, summary, transitions, scope, requested_pages); }

    auto available = host_pool_available_pages_for_scope(scope);
    if (requested_pages <= available) return requested_pages;

    requested_pages = available;
    const auto threshold = target_prefetch_threshold_pages();
    if (threshold > 0 && requested_pages < threshold) return 0;
    return requested_pages;
}

/** @brief 释放 prefetch work 的 modeled host reservation。 */
void HiCacheState::release_prefetch_reservation(PrefetchWorkItem & work) {
    work.requested_host_pages = 0;
    work.reserved_host_pages = 0;
}

/**
 * @brief 释放 prefetch work 持有的所有 modeled runtime resource。
 *
 * SGLang 在 prefetch_from_storage() 开始时 protect_host(last_host_node)，并在
 * terminate / revoke / insufficient-memory 分支释放。模型用独立 synthetic key 把
 * anchor_pages 投影到 page-level host ref，避免和 request 自身的 host ref 生命周期
 * 混在一起。
 */
void HiCacheState::release_prefetch_work_resources(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                                   PrefetchWorkItem & work) {
    release_prefetch_reservation(work);
    if (!work.anchor_ref_key.empty()) {
        release_host_request_ref(fact, summary, transitions, work.anchor_ref_key);
        work.anchor_ref_key.clear();
    }
}

/** @brief 判断 prefetch work 是否仍可能被 checkpoint 推进。 */
bool HiCacheState::prefetch_work_open(const PrefetchWorkItem & work) const {
    return work.state == PrefetchWorkState::Pending || work.state == PrefetchWorkState::Ready;
}

/**
 * @brief 处理 request admission fact。
 *
 * admission 阶段只做 target lookup、request lock/ref 获取和 device pressure 预留，
 * 不把新 page 立即插入 L1。真正的 insert 由 lifecycle anchor 驱动，这样 request
 * 生命周期、unfinished 保护和 capacity cleanup 的顺序保持可审计。
 */
void HiCacheState::apply_request_admission(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    apply_request_context(fact, summary);
    auto tokens = projected_full_path_tokens_for_fact(fact);
    if (tokens.empty()) {
        auto stored = token_store_.request_tokens(fact);
        if (!stored.empty()) tokens = stored;
    }
    if (tokens.empty()) {
        note_missing_projection_if_needed(fact, summary);
        return;
    }
    auto full_pages = pages_for_tokens(fact, tokens);
    if (full_pages.empty()) return;

    const auto request_key = scoped_request_key(fact);
    RequestExecutionState prior_request_state{};
    bool has_prior_request_state = false;
    auto prior_it = request_states_.find(request_key);
    if (prior_it != request_states_.end()) {
        prior_request_state = prior_it->second;
        has_prior_request_state = true;
    }

    auto & radix_tree = device_radix_tree_for_fact(fact);
    auto match = radix_tree.match_page_path(full_pages);
    auto host_match = host_radix_tree_for_fact(fact).match_page_path(full_pages);
    auto matched_pages = match.matched_pages;
    auto device_chain_groups = match.ancestor_page_groups;
    if (has_prior_request_state && prior_request_state.full_pages.size() <= full_pages.size()
        && prior_request_state.matched_device_prefix_pages.size() > matched_pages.size()) {
        matched_pages = slice_pages(full_pages, 0, prior_request_state.matched_device_prefix_pages.size());
        device_chain_groups = prior_request_state.last_device_chain_groups;
    }
    update_request_path_state(fact, full_pages, matched_pages, device_chain_groups, host_match.ancestor_page_groups);

    if (request_key.empty()) return;
    auto & request_state = request_states_[request_key];
    const auto lock_pages = flatten_page_groups(request_state.last_device_chain_groups);
    acquire_device_request_lock(fact, summary, transitions, request_key, lock_pages);
    const auto host_ref_pages = flatten_page_groups(request_state.last_host_chain_groups);
    acquire_host_request_ref(fact, summary, transitions, request_key, host_ref_pages);

    const auto requested_pages = estimate_admission_requested_pages(fact, request_state);
    apply_target_device_pressure(fact, summary, transitions, requested_pages);
    request_state.device_reservation_pages = requested_pages;
    request_state.lifecycle_state = "admitted";
}

/**
 * @brief 处理 request lifecycle anchor。
 *
 * finished request 会 insert 目标 pages 并释放 lock/ref；unfinished request 保留最新
 * ancestor 保护，防止尚未完成的请求路径在后续 cleanup 中被错误回收。
 */
void HiCacheState::apply_request_lifecycle_anchor(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    const auto kind = lower(fact.lifecycle_kind);
    if (!kind.empty() && kind != "finished" && kind != "unfinished") return;
    auto tokens = token_store_.request_tokens(fact);
    if (tokens.empty()) return;
    const auto request_key = scoped_request_key(fact);
    apply_request_lifecycle_insert(fact, summary, transitions);
    clear_device_reservation(request_key);
    auto state_it = request_states_.find(request_key);
    if (state_it == request_states_.end()) return;
    const auto lock_pages = flatten_page_groups(state_it->second.last_device_chain_groups);
    const auto host_ref_pages = flatten_page_groups(state_it->second.last_host_chain_groups);
    if (kind == "unfinished") {
        replace_device_request_lock(fact, summary, transitions, request_key, lock_pages);
        replace_host_request_ref(fact, summary, transitions, request_key, host_ref_pages);
        state_it->second.lifecycle_state = "unfinished";
    }
    else {
        release_device_request_lock(fact, summary, transitions, request_key);
        release_host_request_ref(fact, summary, transitions, request_key);
        request_states_.erase(state_it);
    }
}

/**
 * @brief 处理 request-bound lookup/match anchor。
 *
 * 该 fact 是 target lookup 的确定性锚点：先基于当前 modeled topology 计算 visible
 * prefix，再按 L1/L2/L3 状态执行 target-side promote/touch。source lookup result
 * 不参与决策。
 */
void HiCacheState::apply_request_bound_match_anchor(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    auto tokens = projected_full_path_tokens_for_fact(fact);
    if (tokens.empty()) {
        note_missing_projection_if_needed(fact, summary);
        return;
    }
    auto pages = pages_for_tokens(fact, tokens);
    if (pages.empty()) return;
    token_store_.observe_request_bound_tokens(fact, tokens);

    const auto lookup = match_request_lookup_path(fact, pages);
    update_request_path_state(fact, pages, lookup.device_pages, lookup.device_chain_groups, lookup.host_chain_groups);
    const auto page_size = page_size_for_fact(fact);
    auto restore_device_prefix = [&](size_t page_index) {
        if (page_size == 0) return;
        const auto prefix_pages = slice_pages(pages, 0, page_index + 1);
        const auto prefix_tokens = slice_token_path(tokens, 0, (page_index + 1) * static_cast<size_t>(page_size));
        if (!prefix_pages.empty() && !prefix_tokens.empty()) (void)device_radix_tree_for_fact(fact).insert_path(prefix_tokens, prefix_pages);
    };
    for (const auto index : std::views::iota(size_t{ 0 }, lookup.visible_pages.size())) {
        const auto & page = lookup.visible_pages[index];
        if (state_index_.l1().contains(page)) {
            touch_page("L1", page);
            if (state_index_.l2().contains(page)) touch_page("L2", page);
            restore_device_prefix(index);
            continue;
        }
        if (state_index_.l2().contains(page)) {
            touch_page("L2", page);
            restore_device_prefix(index);
            add_resident(fact, summary, transitions, "L1", page);
            continue;
        }
        if (state_index_.l3().contains(page)) {
            restore_device_prefix(index);
            add_host_visible_page(fact, summary, transitions, page);
            add_resident(fact, summary, transitions, "L1", page);
            if (config_.prefetch_policy == "wait_complete" && state_index_.prefetch_planned().contains(page))
                mark_prefetch_ready(fact, summary, transitions, page);
        }
    }
    const auto post_lookup = match_request_lookup_path(fact, pages);
    update_request_path_state(fact, pages, post_lookup.device_pages, post_lookup.device_chain_groups, post_lookup.host_chain_groups);
    enforce_host_capacity(fact, summary, transitions);
    enforce_capacity(fact, summary, transitions, "L1");
}

/**
 * @brief 在 lifecycle 边界插入 request full path 的缺失 device pages。
 *
 * device radix topology 只说明路径曾被建模过，不等价于当前 L1 residency。插入时按
 * target-visible L1 prefix 计算缺失 page：write-through 与 selective 共享 hit-count
 * backup 逻辑，write-back 只把新 device page 标 dirty，等 L1 eviction 再同步写回。
 */
void HiCacheState::apply_request_lifecycle_insert(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    auto tokens = projected_full_path_tokens_for_fact(fact);
    if (tokens.empty()) {
        tokens = token_store_.request_tokens(fact);
        if (!tokens.empty()) {
            const auto desired = page_aligned_token_count(tokens.size(), page_size_for_fact(fact));
            tokens = slice_token_path(tokens, 0, desired);
        }
    }
    if (tokens.empty()) {
        note_missing_projection_if_needed(fact, summary);
        return;
    }
    auto full_pages = pages_for_tokens(fact, tokens);
    if (full_pages.empty()) return;
    token_store_.set_request_tokens(fact, tokens);

    auto & radix_tree = device_radix_tree_for_fact(fact);
    const auto pre_insert_lookup = match_request_lookup_path(fact, full_pages);
    auto inserted_pages = suffix_pages(full_pages, pre_insert_lookup.device_pages.size());
    (void)radix_tree.insert_path(tokens, full_pages);
    for (const auto & page : inserted_pages) { apply_new_device_page_write_policy(fact, summary, transitions, page); }
    apply_write_policy_hit_counts(fact, full_pages, summary, transitions);
    const auto post_lookup = match_request_lookup_path(fact, full_pages);
    update_request_path_state(fact, full_pages, post_lookup.device_pages, post_lookup.device_chain_groups, post_lookup.host_chain_groups);
    enforce_capacity(fact, summary, transitions, "L1");
    enforce_host_capacity(fact, summary, transitions);
}

/**
 * @brief 处理 prefetch decision fact，并创建 modeled async work。
 *
 * 三种 storage prefetch policy 都先按 SGLang prefetch_from_storage() 建立同一个
 * work：非 device/L2 memory prefix 之后的 page-aligned suffix 是 planned
 * prefetch_key，host allocation 预留 memory，storage hit query 再把 pages 截断为
 * target-derived 连续命中前缀。policy 差异只在 checkpoint 是否能 terminate，而
 * 不是是否绕过 storage hit / host protection。
 */
void HiCacheState::apply_prefetch_decision(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    auto tokens = projected_full_path_tokens_for_fact(fact);
    if (tokens.empty()) {
        note_missing_projection_if_needed(fact, summary);
        return;
    }
    auto full_pages = pages_for_tokens(fact, tokens);
    if (full_pages.empty()) return;
    token_store_.set_request_tokens(fact, tokens);
    if (fact.request_id.empty()) return;

    const auto lookup = match_request_lookup_path(fact, full_pages);
    const auto host_match = host_radix_tree_for_fact(fact).match_page_path(full_pages);
    const auto host_memory_pages = contiguous_resident_prefix(full_pages, host_match.matched_pages.size(), false, true, false);
    const auto memory_prefix_pages = contiguous_resident_prefix(full_pages, std::max(lookup.device_pages.size(), host_memory_pages.size()), true, true, false);
    const auto suffix = suffix_pages(full_pages, memory_prefix_pages.size());
    auto anchor_pages = memory_prefix_pages;
    auto planned_pages = suffix;
    if (planned_pages.empty()) return;

    auto storage_pages = target_prefetch_storage_pages(host_match.matched_pages, memory_prefix_pages.size());
    const auto requested_host_pages = static_cast<uint64_t>(planned_pages.size());
    if (requested_host_pages < target_prefetch_threshold_pages()) return;

    const auto key = scoped_request_key(fact);
    auto prior = async_state_.prefetch_by_request.find(key);
    if (prior != async_state_.prefetch_by_request.end()) {
        if (prefetch_work_open(prior->second)) suppress_prefetch_work(fact, summary, transitions, prior->second);
        else release_prefetch_work_resources(fact, summary, transitions, prior->second);
        async_state_.prefetch_by_request.erase(prior);
    }

    const auto scope = normalized_scope(fact);
    const auto limit = target_prefetch_capacity_limit_pages();
    if (active_prefetch_requested_pages_for_scope(scope) >= limit) return;

    const auto reserved_host_pages = reserve_host_pages_for_prefetch(fact, summary, transitions, scope, requested_host_pages);
    if (reserved_host_pages == 0) return;
    if (planned_pages.size() > reserved_host_pages) planned_pages.resize(static_cast<size_t>(reserved_host_pages));
    if (storage_pages.size() > reserved_host_pages) storage_pages.resize(static_cast<size_t>(reserved_host_pages));

    const auto enqueue_epoch = ++async_state_.scheduler_epoch;
    PrefetchWorkItem work{
        key,
        key + ":prefetch_anchor:" + std::to_string(enqueue_epoch),
        scope,
        anchor_pages,
        planned_pages,
        storage_pages,
        enqueue_epoch,
        0,
        fact.ts,
        0,
        fact.priority,
        fact.ignore_eos,
        requested_host_pages,
        reserved_host_pages,
        PrefetchWorkState::Pending,
    };
    async_state_.issued_work_units += static_cast<uint64_t>(planned_pages.size());
    acquire_host_request_ref(fact, summary, transitions, work.anchor_ref_key, work.anchor_pages);
    async_state_.prefetch_by_request[key] = std::move(work);

    for (const auto & page : planned_pages) { mark_prefetch_planned(fact, summary, transitions, page); }
}

/**
 * @brief 处理 prefetch checkpoint fact。
 *
 * checkpoint 只推进 target-modeled async lifecycle。storage miss / partial hit 已在
 * decision 阶段按 target storage-known topology 截断为 work.pages；checkpoint 阶段
 * 只决定这个 work 是否能 terminate 并落入 host-visible state。进入 applied /
 * suppressed / late 的 work 会立即从 ongoing map 移除，避免后续 checkpoint 重新打开
 * 已终止的 prefetch 生命周期。
 */
void HiCacheState::apply_prefetch_check_point(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    if (fact.request_id.empty()) return;
    const auto key = scoped_request_key(fact);
    auto it = async_state_.prefetch_by_request.find(key);
    if (it == async_state_.prefetch_by_request.end() || (it->second.pages.empty() && it->second.planned_pages.empty())) return;
    auto & work = it->second;
    auto erase_work = [&]() { async_state_.prefetch_by_request.erase(it); };
    auto release_and_erase = [&]() {
        release_prefetch_work_resources(fact, summary, transitions, work);
        erase_work();
    };
    auto suppress_and_erase = [&]() {
        suppress_prefetch_work(fact, summary, transitions, work);
        erase_work();
    };
    auto apply_ready_and_erase = [&]() {
        work.state = PrefetchWorkState::Ready;
        complete_prefetch_ready_checkpoint(fact, summary, transitions, work);
        apply_host_visibility_for_ready_work(fact, summary, transitions, work);
        erase_work();
    };

    if (!prefetch_work_open(work)) {
        release_and_erase();
        return;
    }

    async_state_.checkpoint_index++;
    work.checkpoint_epoch = async_state_.checkpoint_index;
    work.last_checkpoint_ts = fact.ts;

    if (work.pages.empty() || work.pages.size() < target_prefetch_threshold_pages()) {
        suppress_and_erase();
        return;
    }

    if (config_.prefetch_policy == "wait_complete") {
        apply_ready_and_erase();
        return;
    }

    if (config_.prefetch_policy == "best_effort") {
        if (work.state == PrefetchWorkState::Pending) {
            if (best_effort_ready_budget_available(work)) apply_ready_and_erase();
            else suppress_and_erase();
            return;
        }
        if (terminal_prefetch_checkpoint(fact) && work.state == PrefetchWorkState::Ready) {
            suppress_and_erase();
            return;
        }
        return;
    }

    if (config_.prefetch_policy != "timeout") {
        apply_ready_and_erase();
        return;
    }

    if (terminal_prefetch_checkpoint(fact)) {
        apply_ready_and_erase();
        return;
    }
    if (!config_.prefetch_timeout_configured || !prefetch_timeout_elapsed(fact, work)) return;
    mark_prefetch_work_late(fact, summary, transitions, work);
    erase_work();
}

/**
 * @brief 根据 write policy 维护 page hit count 与 selective write-through。
 *
 * 计数只对 target topology 中存在的 page 生效。达到 threshold 后 page 进入
 * host-visible/storage-backed 状态，并清理 dirty 标记。
 */
void HiCacheState::apply_write_policy_hit_counts(const HiCacheFact & fact, const std::vector<std::string> & full_pages, HiCacheSummary & summary,
                                                 std::vector<HiCacheStateTransition> & transitions) {
    if (!target_write_count_enabled()) return;
    const auto threshold = target_write_through_threshold();
    if (threshold == 0) return;
    std::set<std::string> counted;
    for (const auto & page : full_pages) {
        if (page.empty() || !counted.insert(page).second) continue;
        if (state_index_.evicted().contains(page)) continue;
        if (!state_index_.l1().contains(page) && !device_radix_tree_for_fact(fact).contains_page(page)) continue;
        auto before = transition_state_digest();
        const auto hit_count = state_index_.increment_hit_count(fact.cache_scope, page);
        record_transition(fact, summary, transitions, "increment_hit_count", "", page, before);
        if (hit_count < threshold) continue;
        commit_host_backup_page(fact, summary, transitions, page, true);
    }
}

/**
 * @brief 对新进入 L1 的 page 应用 write policy 的 device-side 语义。
 *
 * SGLang 的 write-through 和 selective 都走 `_inc_hit_count()`；差异只在 threshold。
 * write-back 则跳过 hit count，并把新 page 保留为 dirty，等 device eviction 时同步
 * `write_backup(..., write_back=True)`。
 */
void HiCacheState::apply_new_device_page_write_policy(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                                      const std::string & page) {
    if (page.empty()) return;
    add_resident(fact, summary, transitions, "L1", page);
    if (target_write_back_enabled()) { mark_dirty(fact, summary, transitions, page); }
    else { clear_dirty(fact, summary, transitions, page); }
}

/**
 * @brief 在 L1 eviction 前应用 write policy 对未 backup page 的处置。
 *
 * write-back 必须同步完成 modeled writeback 后再移除 device residency；write-through
 * / selective 如果 page 仍未 backuped，则对齐 SGLang regular eviction：只移除 device
 * residency，不强行生成 host-visible 副本。
 */
void HiCacheState::apply_device_eviction_write_policy(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                                      const std::string & page) {
    if (page.empty()) return;
    if (state_index_.backuped().contains(page)) {
        clear_dirty(fact, summary, transitions, page);
        return;
    }
    if (target_write_back_enabled()) {
        summary.dirty_eviction_events++;
        enqueue_writeback_page(fact, summary, transitions, page);
        complete_writeback_page(fact, summary, transitions, page);
        return;
    }
    clear_dirty(fact, summary, transitions, page);
}

/**
 * @brief 判断 best-effort work 是否具备 ready 条件。
 *
 * best-effort 的 stop policy 允许 checkpoint 立即 terminate；ready 仍必须建立在
 * controller storage hit query 的 target-derived 结果上，低于 threshold 的 work 会
 * 在 checkpoint 被视为 revoked/suppressed。
 */
bool HiCacheState::best_effort_ready_budget_available(const PrefetchWorkItem & work) const {
    if (work.pages.empty()) return false;
    if (work.pages.size() < target_prefetch_threshold_pages()) return false;
    return work.enqueue_epoch > 0 && work.checkpoint_epoch > 0;
}

/**
 * @brief 判断 timeout policy 是否已经达到目标 timeout 窗口。
 *
 * SGLang timeout 按 operation.hash_value 的 page 数线性计算；模型使用 storage hit
 * 截断后的 work.pages 作为 operation.hash_value 的 target 投影。
 */
bool HiCacheState::prefetch_timeout_elapsed(const HiCacheFact & fact, const PrefetchWorkItem & work) const {
    if (fact.ts < work.enqueue_ts) return false;
    const auto page_size = page_size_for_fact(fact);
    const auto token_count = static_cast<uint64_t>(work.pages.size()) * page_size;
    double timeout_sec = config_.prefetch_timeout_base_sec + config_.prefetch_timeout_per_ki_token_sec * static_cast<double>(token_count) / 1024.0;
    if (config_.prefetch_timeout_max_sec > 0.0) timeout_sec = std::min(timeout_sec, config_.prefetch_timeout_max_sec);
    const double elapsed_sec = static_cast<double>(fact.ts - work.enqueue_ts) / 1000000.0;
    return elapsed_sec >= timeout_sec;
}

/**
 * @brief 从 host radix match 中筛选 prefetch storage hit 的连续前缀。
 *
 * SGLang `_storage_hit_query()` 会从 prefetch_key 起点顺序查询 storage backend，遇到
 * 第一个 miss 就停止。模型不消费 source completed_tokens / hit pages，而是要求候选
 * page 已经在 target state 中 storage-known，且当前不在 L1/L2。
 */
std::vector<std::string> HiCacheState::target_prefetch_storage_pages(const std::vector<std::string> & matched_pages, size_t first_page_index) const {
    std::vector<std::string> pages;
    if (first_page_index >= matched_pages.size()) return pages;
    pages.reserve(matched_pages.size() - first_page_index);
    for (const auto index : std::views::iota(first_page_index, matched_pages.size())) {
        const auto & page = matched_pages[index];
        if (page.empty()) break;
        if (state_index_.l2().contains(page)) break;
        if (state_index_.l1().contains(page)) break;
        if (!host_state_.storage_known_pages.contains(page)) break;
        pages.push_back(page);
    }
    return pages;
}

/** @brief 判断 checkpoint kind 是否表示 prefetch 生命周期的终态边界。 */
bool HiCacheState::terminal_prefetch_checkpoint(const HiCacheFact & fact) const {
    const auto kind = lower(fact.check_kind);
    return kind == "complete" || kind == "completed" || kind == "terminate" || kind == "terminated" || kind == "final" || kind == "finalize";
}

/**
 * @brief 将 best-effort work 标记为 ready，但暂不强行可见。
 *
 * ready_not_visible_pages 记录“异步工作已经完成、但 visibility 尚未应用”的中间态，
 * 便于 checkpoint policy 明确控制何时进入 L2/L3。
 */
void HiCacheState::complete_prefetch_ready_checkpoint(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                                      PrefetchWorkItem & work) {
    if (work.state != PrefetchWorkState::Ready) return;
    for (const auto & page : work.pages) {
        mark_prefetch_ready(fact, summary, transitions, page);
        host_state_.ready_not_visible_pages.insert(page);
    }
    async_state_.completed_work_units += static_cast<uint64_t>(work.pages.size());
}

/**
 * @brief 将 ready prefetch work 应用到 host-visible/storage-backed 状态。
 *
 * host topology 使用 anchor_pages + pages 恢复完整路径；每个 page 进入 L2、L3、
 * backuped，并标记为 evicted，表示它是可被 host cleanup 回收的 storage-backed page。
 */
void HiCacheState::apply_host_visibility_for_ready_work(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                                        PrefetchWorkItem & work) {
    if (work.state != PrefetchWorkState::Ready) return;
    std::vector<std::string> host_path = work.anchor_pages;
    host_path.insert(host_path.end(), work.pages.begin(), work.pages.end());
    insert_host_path_topology(fact, host_path.empty() ? work.pages : host_path);
    for (const auto & page : work.pages) {
        if (page.empty()) continue;
        commit_host_backup_page(fact, summary, transitions, page, true);
        mark_evicted(fact, summary, transitions, page);
        host_state_.ready_not_visible_pages.erase(page);
    }
    const std::set<std::string> ready_pages(work.pages.begin(), work.pages.end());
    for (const auto & page : work.planned_pages) {
        if (page.empty() || ready_pages.contains(page)) continue;
        mark_prefetch_suppressed(fact, summary, transitions, page);
        host_state_.ready_not_visible_pages.erase(page);
    }
    release_prefetch_work_resources(fact, summary, transitions, work);
    enforce_host_capacity(fact, summary, transitions);
    work.state = PrefetchWorkState::Applied;
}

/**
 * @brief suppress 尚未 applied 的 prefetch work。
 *
 * 已 ready 的 page 不会被覆盖成 suppressed；reservation 必须释放，避免后续
 * best-effort capacity limit 被已经终止的 work 占用。
 */
void HiCacheState::suppress_prefetch_work(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                          PrefetchWorkItem & work) {
    if (!prefetch_work_open(work)) return;
    const auto & pages = work.planned_pages.empty() ? work.pages : work.planned_pages;
    for (const auto & page : pages) {
        if (!state_index_.prefetch_ready().contains(page)) mark_prefetch_suppressed(fact, summary, transitions, page);
        host_state_.ready_not_visible_pages.erase(page);
    }
    release_prefetch_work_resources(fact, summary, transitions, work);
    work.state = PrefetchWorkState::Suppressed;
}

/**
 * @brief 将 timeout prefetch work 标记为 late。
 *
 * late 是 target timeout policy 的结果，不代表 source async worker 一定失败。
 * 已经 ready 的 page 保持 ready，不会被 late 覆盖。
 */
void HiCacheState::mark_prefetch_work_late(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                           PrefetchWorkItem & work) {
    if (!prefetch_work_open(work)) return;
    const auto & pages = work.planned_pages.empty() ? work.pages : work.planned_pages;
    for (const auto & page : pages) {
        if (!state_index_.prefetch_ready().contains(page)) mark_prefetch_late(fact, summary, transitions, page);
        host_state_.ready_not_visible_pages.erase(page);
    }
    release_prefetch_work_resources(fact, summary, transitions, work);
    work.state = PrefetchWorkState::Late;
}

/**
 * @brief 添加 tier residency，并在真实变化时记录 transition。
 *
 * 所有 resident set 写入都经过这里，保证 LRU touch、state index 和 transition trace
 * 的顺序一致。
 */
void HiCacheState::add_resident(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, const std::string & tier,
                                const std::string & page) {
    auto before = transition_state_digest();
    if (state_index_.add_resident(tier, page)) record_transition(fact, summary, transitions, "add_" + lower(tier) + "_resident", tier, page, before);
}

/**
 * @brief 将 page 纳入 host-visible 状态。
 *
 * host-visible 同时意味着 L2 resident、backuped/storage-known 和 host radix topology
 * 可恢复；这些联动集中在一个 helper 中，避免不同 handler 写出不一致的 host state。
 */
void HiCacheState::add_host_visible_page(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                         const std::string & page) {
    if (page.empty()) return;
    insert_host_page_topology_from_device(fact, page);
    auto before = transition_state_digest();
    if (state_index_.add_resident("L2", page)) record_transition(fact, summary, transitions, "add_l2_resident", "L2", page, before);
    host_state_.host_visible_pages.insert(page);
    mark_backuped(fact, summary, transitions, page);
}

/**
 * @brief 提交一次同步化的 host backup 结果。
 *
 * 该 helper 表示 `write_backup()` / `writing_check()` 已经完成后的语义结果，不建模
 * ack 时序。所有 write-through、selective threshold 和 write-back eviction 的
 * backup 结果都走这里，保证 L2、backuped、storage-known 和可选 L3 resident 的联动
 * 一致；dirty cleanup 由 add_host_visible_page() -> mark_backuped() 统一完成。
 */
void HiCacheState::commit_host_backup_page(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                           const std::string & page, bool storage_resident) {
    add_host_visible_page(fact, summary, transitions, page);
    if (storage_resident) add_resident(fact, summary, transitions, "L3", page);
}

/**
 * @brief 从 host-visible 状态移除 page。
 *
 * 移除 L2 residency 时必须同步清理 backuped 和 ready-not-visible 标记；storage_known
 * topology 不在这里删除，因为它表示 target 已经知道该 storage-backed path。
 */
void HiCacheState::remove_host_visible_page(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                            const std::string & page) {
    if (page.empty()) return;
    auto before = transition_state_digest();
    if (state_index_.remove_resident("L2", page)) record_transition(fact, summary, transitions, "remove_l2_resident", "L2", page, before);
    host_state_.host_visible_pages.erase(page);
    host_state_.ready_not_visible_pages.erase(page);
    clear_backuped(fact, summary, transitions, page);
}

/** @brief 移除 tier residency，并在真实变化时记录 transition。 */
void HiCacheState::remove_resident(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                   const std::string & tier, const std::string & page) {
    auto before = transition_state_digest();
    if (state_index_.remove_resident(tier, page)) { record_transition(fact, summary, transitions, "remove_" + lower(tier) + "_resident", tier, page, before); }
}

/** @brief 标记 write-back page 为 dirty。 */
void HiCacheState::mark_dirty(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, const std::string & page) {
    auto before = transition_state_digest();
    if (state_index_.mark_dirty(page)) record_transition(fact, summary, transitions, "mark_dirty", "", page, before);
}

/** @brief 清理 dirty 标记，通常发生在 write-through 或 modeled writeback 完成后。 */
void HiCacheState::clear_dirty(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                               const std::string & page) {
    auto before = transition_state_digest();
    if (state_index_.clear_dirty(page)) record_transition(fact, summary, transitions, "clear_dirty", "", page, before);
}

/**
 * @brief 标记 page 已经具备 storage-backed 副本。
 *
 * backuped 会同步进入 storage_known_pages，并补齐 host topology；后续 best-effort
 * prefetch 只能从这些 target-known storage page 中选择候选。
 */
void HiCacheState::mark_backuped(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                 const std::string & page) {
    host_state_.storage_known_pages.insert(page);
    insert_host_page_topology_from_device(fact, page);
    auto before = transition_state_digest();
    if (state_index_.mark_backuped(page)) record_transition(fact, summary, transitions, "mark_backuped", "", page, before);
    clear_dirty(fact, summary, transitions, page);
}

/** @brief 清理 backuped 标记，但不删除 storage-known topology 记忆。 */
void HiCacheState::clear_backuped(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                  const std::string & page) {
    auto before = transition_state_digest();
    if (state_index_.clear_backuped(page)) record_transition(fact, summary, transitions, "clear_backuped", "", page, before);
}

/** @brief 标记 page 已从 L1 角度 evicted，可作为 host cleanup 候选。 */
void HiCacheState::mark_evicted(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                const std::string & page) {
    auto before = transition_state_digest();
    if (state_index_.mark_evicted(page)) record_transition(fact, summary, transitions, "mark_evicted", "", page, before);
}

/** @brief 清理 evicted 标记，通常表示 page 已重新进入 L1 或已被 host 回收。 */
void HiCacheState::clear_evicted(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                 const std::string & page) {
    auto before = transition_state_digest();
    if (state_index_.clear_evicted(page)) record_transition(fact, summary, transitions, "clear_evicted", "", page, before);
}

/** @brief 标记 page 被 request lock 保护。 */
void HiCacheState::mark_locked(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                               const std::string & page) {
    auto before = transition_state_digest();
    if (state_index_.mark_locked(page)) record_transition(fact, summary, transitions, "mark_locked", "", page, before);
}

/** @brief 清理 page 的 request lock 保护标记。 */
void HiCacheState::clear_locked(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                const std::string & page) {
    auto before = transition_state_digest();
    if (state_index_.clear_locked(page)) record_transition(fact, summary, transitions, "clear_locked", "", page, before);
}

/** @brief 标记 page 已被 prefetch decision 规划。 */
void HiCacheState::mark_prefetch_planned(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                         const std::string & page) {
    auto before = transition_state_digest();
    if (state_index_.mark_prefetch_planned(page)) record_transition(fact, summary, transitions, "mark_prefetch_planned", "", page, before);
}

/** @brief 标记 page 的 prefetch work 已 ready，并清理 late/suppressed 互斥状态。 */
void HiCacheState::mark_prefetch_ready(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                       const std::string & page) {
    auto before = transition_state_digest();
    if (state_index_.mark_prefetch_ready(page)) record_transition(fact, summary, transitions, "mark_prefetch_ready", "", page, before);
}

/** @brief 标记 page 的 prefetch work 超时变 late。 */
void HiCacheState::mark_prefetch_late(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                      const std::string & page) {
    auto before = transition_state_digest();
    if (state_index_.mark_prefetch_late(page)) record_transition(fact, summary, transitions, "mark_prefetch_late", "", page, before);
}

/** @brief 标记 page 的 prefetch work 被 suppress。 */
void HiCacheState::mark_prefetch_suppressed(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                            const std::string & page) {
    auto before = transition_state_digest();
    if (state_index_.mark_prefetch_suppressed(page)) record_transition(fact, summary, transitions, "mark_prefetch_suppressed", "", page, before);
}

/** @brief 更新 tier 的 LRU touch order，不产生 transition。 */
void HiCacheState::touch_page(const std::string & tier, const std::string & page) { state_index_.touch_page(tier, page); }

/**
 * @brief 将 page 放入 modeled writeback queue。
 *
 * queue 按 cache_scope 隔离；transition 只表示模型中出现 pending writeback，不代表
 * DAG 中新增了异步节点。
 */
void HiCacheState::enqueue_writeback_page(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                          const std::string & page) {
    const auto scope = normalized_scope(fact);
    auto before = transition_state_digest();
    if (pending_writeback_pages_by_scope_[scope].insert(page).second) record_transition(fact, summary, transitions, "enqueue_writeback", "", page, before);
}

/**
 * @brief 完成 modeled writeback，并使 page 进入 host-visible/storage-backed 状态。
 *
 * 当前状态模型把 write-back capacity eviction 建模为同步等待 ACK 后再移除 device
 * block，因此 enqueue 和 complete 会在同一状态迁移流程中连续出现。
 */
void HiCacheState::complete_writeback_page(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                           const std::string & page) {
    const auto scope = normalized_scope(fact);
    auto it = pending_writeback_pages_by_scope_.find(scope);
    if (it != pending_writeback_pages_by_scope_.end()) {
        auto before = transition_state_digest();
        const bool removed = it->second.erase(page) > 0;
        if (it->second.empty()) pending_writeback_pages_by_scope_.erase(it);
        if (removed) record_transition(fact, summary, transitions, "complete_writeback", "", page, before);
    }
    commit_host_backup_page(fact, summary, transitions, page, true);
}

uint64_t HiCacheState::evict_host_pages(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                        uint64_t page_count) {
    return evict_host_pages_for_scope(fact, summary, transitions, normalized_scope(fact), page_count);
}

/**
 * @brief 按 target host policy 回收指定 scope 的 host-visible pages。
 *
 * host cleanup 只回收已经 evicted 且未被 host ref / device lock 保护的 page。优先按
 * radix leaf group 成组选择 victim，保持 SGLang radix block 的拓扑语义；如果找不到
 * group，再退回到 LRU order 上的单 leaf group。
 */
uint64_t HiCacheState::evict_host_pages_for_scope(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                                  const std::string & scope, uint64_t page_count) {
    auto * pages = tier_set("L2");
    auto * order = touch_order_for_tier("L2");
    if (!pages || !order || page_count == 0) return 0;
    auto & host_tree = host_radix_tree_for_fact(fact);
    uint64_t removed = 0;
    std::set<std::string> skipped_protected;
    while (removed < page_count) {
        std::set<std::string> host_pages;
        std::set<std::string> evicted_pages;
        std::set<std::string> protected_pages;
        for (const auto & page : *pages) {
            if (!page_in_scope(page, scope)) continue;
            host_pages.insert(page);
            if (state_index_.evicted().contains(page)) evicted_pages.insert(page);
            if (host_state_.ref_count_by_page.contains(page) || state_index_.locked().contains(page)) protected_pages.insert(page);
        }
        if (evicted_pages.empty()) break;

        auto candidate_groups = host_tree.host_eviction_leaf_groups(host_pages, evicted_pages, protected_pages);
        std::vector<std::string> victims;
        if (!candidate_groups.empty()) victims = earliest_lru_group(candidate_groups, *order);
        if (victims.empty()) {
            auto victim_it = std::ranges::find_if(*order, [&](const auto & page) {
                return evicted_pages.contains(page) && !protected_pages.contains(page) && !skipped_protected.contains(page);
            });
            if (victim_it == order->end()) break;
            const auto victim = *victim_it;
            victims = host_tree.leaf_group_for_page(victim);
            if (victims.empty()) victims = { victim };
        }

        bool protected_group = false;
        for (const auto & page : victims) {
            if (protected_pages.contains(page)) {
                protected_group = true;
                break;
            }
        }
        if (protected_group) {
            for (const auto & page : victims) {
                if (!page.empty()) skipped_protected.insert(page);
            }
            continue;
        }

        for (const auto & page : victims) {
            if (!pages->contains(page) || !state_index_.evicted().contains(page)) continue;
            std::erase(*order, page);
            remove_host_visible_page(fact, summary, transitions, page);
            clear_evicted(fact, summary, transitions, page);
            removed++;
        }
    }
    return removed;
}

/**
 * @brief 强制 L2 不超过 target capacity。
 *
 * host capacity 按 scope 独立检查。若一轮 cleanup 没有移除任何 page，说明剩余 page
 * 都受保护或拓扑不可安全回收，循环必须停止以保持状态机确定性。
 */
void HiCacheState::enforce_host_capacity(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    if (config_.l2_capacity_pages == 0) return;
    const auto scope = normalized_scope(fact);
    while (tier_size_for_scope("L2", scope) > config_.l2_capacity_pages) {
        const auto before = tier_size_for_scope("L2", scope);
        evict_host_pages(fact, summary, transitions, before - config_.l2_capacity_pages);
        if (tier_size_for_scope("L2", scope) == before) break;
    }
}

/**
 * @brief 回收指定 tier 的 LRU pages。
 *
 * L1 回收按 device radix leaf group 选择 victim，并跳过 locked group；write policy
 * 只在移除 device residency 前处理未 backup page：write-back 同步写回后再 demote，
 * write-through/selective 未 backup page 走 regular device eviction。L2 回收委托给
 * host cleanup，因为 host 有额外的 evicted/ref/topology 约束。
 */
void HiCacheState::evict_lru_pages(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                   const std::string & tier, uint64_t page_count) {
    if (tier == "L2") {
        evict_host_pages(fact, summary, transitions, page_count);
        return;
    }
    auto * pages = tier_set(tier);
    auto * order = touch_order_for_tier(tier);
    if (!pages || !order || page_count == 0) return;
    const auto scope = normalized_scope(fact);
    auto & radix_tree = device_radix_tree_for_fact(fact);
    uint64_t removed = 0;
    std::set<std::string> skipped_locked;
    while (removed < page_count) {
        std::vector<std::string> victims;
        bool dynamic_device_tree_ready = false;
        if (tier == "L1") {
            std::set<std::string> device_pages;
            std::set<std::string> locked_pages;
            for (const auto & page : *pages) {
                if (!page_in_scope(page, scope)) continue;
                device_pages.insert(page);
                if (radix_tree.contains_page(page)) dynamic_device_tree_ready = true;
                if (state_index_.locked().contains(page)) locked_pages.insert(page);
            }
            if (dynamic_device_tree_ready) {
                auto candidate_groups = radix_tree.device_eviction_leaf_groups(device_pages, locked_pages);
                victims = earliest_lru_group(candidate_groups, *order);
            }
        }
        if (victims.empty()) {
            auto victim_it = std::ranges::find_if(*order, [&](const auto & page) {
                return pages->contains(page) && page_in_scope(page, scope) && !skipped_locked.contains(page);
            });
            if (victim_it == order->end()) break;
            auto victim = *victim_it;
            if (!pages->contains(victim)) continue;
            victims = radix_tree.leaf_group_for_page(victim);
            if (victims.empty()) victims = { victim };
        }
        bool locked_group = false;
        for (const auto & page : victims) {
            if (state_index_.locked().contains(page)) {
                locked_group = true;
                break;
            }
        }
        if (locked_group) {
            for (const auto & page : victims) {
                if (!page.empty()) skipped_locked.insert(page);
            }
            continue;
        }
        for (const auto & page : victims) {
            if (!pages->contains(page)) continue;
            std::erase(*order, page);
            if (tier == "L1") apply_device_eviction_write_policy(fact, summary, transitions, page);
            remove_resident(fact, summary, transitions, tier, page);
            if (tier == "L1") {
                if (state_index_.backuped().contains(page)) mark_evicted(fact, summary, transitions, page);
                else clear_evicted(fact, summary, transitions, page);
            }
            else clear_evicted(fact, summary, transitions, page);
            removed++;
        }
    }
}

/**
 * @brief 对指定 tier 执行 target capacity enforcement。
 *
 * 该函数只根据 target capacity、scope-local tier size 和 modeled victim policy 工作。
 * 如果无法继续减少 size，说明剩余 page 受保护或缺少可安全回收的拓扑边界。
 */
void HiCacheState::enforce_capacity(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                    const std::string & tier) {
    auto capacity = capacity_for_tier(tier);
    if (capacity == 0) return;
    auto * pages = tier_set(tier);
    auto * order = touch_order_for_tier(tier);
    if (!pages || !order) return;
    const auto scope = normalized_scope(fact);
    while (tier_size_for_scope(tier, scope) > capacity && !order->empty()) {
        const auto before = tier_size_for_scope(tier, scope);
        evict_lru_pages(fact, summary, transitions, tier, before - capacity);
        if (tier_size_for_scope(tier, scope) == before) break;
    }
}

/** @brief 根据配置决定是否为 transition 记录 before/after state digest。 */
std::string HiCacheState::transition_state_digest() const { return config_.emit_state_digests ? digest() : std::string{}; }

/**
 * @brief 记录一次有效 state mutation。
 *
 * transition row 保留 source fact 的角色、request、operation、scope 和时间戳，便于把
 * modeled state 变化追溯回输入 fact。该函数不修改 DAG。
 */
void HiCacheState::record_transition(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                     const std::string & kind, const std::string & tier, const std::string & page, const std::string & before_digest) {
    summary.state_transition_count++;
    summary.transitions_by_kind[kind]++;
    transitions.push_back(HiCacheStateTransition{
        "hicache_transition_" + std::to_string(summary.state_transition_count),
        kind,
        fact.role,
        fact.request_id,
        fact.operation_id,
        fact.event_name,
        fact.cache_scope,
        fact.ts,
        fact.source_event_index,
        tier,
        std::vector<std::string>{ page },
        before_digest,
        transition_state_digest(),
    });
}

HiCacheStateModel::HiCacheStateModel(HiCacheConfig config) : config_(std::move(config)), state_(config_) {}

/**
 * @brief 扫描 DAG trace，运行 HiCache target-state 状态模型。
 *
 * runner 先收集 token dictionary，再解析所有 HiCache fact，并用稳定排序建立 target
 * processing order。只有通过 router 的 atomic invariant end event 才会进入状态机；
 * source_actual、timing_observation、oracle/debug 只会作为非模型事实被跳过。
 */
HiCacheSummary HiCacheStateModel::run(DagGraph & graph) {
    HiCacheSummary summary{};
    summary.target_config = config_;
    if (!config_.enabled) {
        summary.status = "disabled";
        return summary;
    }

    for (const auto & node : graph.nodes()) {
        const auto & event = graph.event_for_node(node.id);
        if (fact_parser_.is_hicache_event(event)) fact_parser_.observe_token_dictionaries(event);
    }

    std::vector<HiCacheFact> facts;
    for (const auto & node : graph.nodes()) {
        const auto & event = graph.event_for_node(node.id);
        if (!fact_parser_.is_hicache_event(event)) continue;
        auto fact = fact_parser_.parse(node.id, event);
        summary.input_hicache_events++;
        summary.events_by_role[fact.role]++;
        facts.push_back(std::move(fact));
    }

    std::ranges::stable_sort(facts, [](const HiCacheFact & left, const HiCacheFact & right) {
        if (left.ts != right.ts) return left.ts < right.ts;
        if (left.cache_scope != right.cache_scope) return left.cache_scope < right.cache_scope;
        if (left.seq_no > 0 && right.seq_no > 0 && left.seq_no != right.seq_no) return left.seq_no < right.seq_no;
        if ((left.seq_no == 0) != (right.seq_no == 0)) return left.seq_no > 0;
        if (left.is_start != right.is_start) return left.is_start && !right.is_start;
        return left.source_event_index < right.source_event_index;
    });

    for (const auto & fact : facts) {
        const auto route = route_hicache_fact(fact);
        if (!route.model_fact) {
            summary.skipped_non_invariant_events++;
            continue;
        }
        if (!route.known_role) {
            summary.missing_invariant_facts["unknown_invariant_role"]++;
            continue;
        }
        if (!fact.is_end) continue;

        const auto effective_page_size = config_.page_size > 0 ? config_.page_size : fact.source_page_size;
        const auto missing_fields = hicache_required_fact_errors(fact, route.role, effective_page_size);
        if (!missing_fields.empty()) {
            for (const auto & key : missing_fields) summary.missing_invariant_facts[key]++;
            continue;
        }
        if (!hicache_fact_role_implemented(route.role)) {
            summary.missing_invariant_facts["unimplemented_invariant_role." + hicache_fact_role_name(route.role)]++;
            continue;
        }

        summary.processed_hicache_events++;
        summary.processed_events_by_role[fact.role]++;
        auto transitions = state_.apply_fact(fact, route.role, summary);
        summary.transition_trace.insert(summary.transition_trace.end(), transitions.begin(), transitions.end());
    }
    auto final_transitions = state_.finalize(summary);
    summary.transition_trace.insert(summary.transition_trace.end(), final_transitions.begin(), final_transitions.end());

    summary.l1_resident_pages = sorted_vector(state_.l1());
    summary.l2_resident_pages = sorted_vector(state_.l2());
    summary.l3_resident_pages = sorted_vector(state_.l3());
    summary.dirty_pages = sorted_vector(state_.dirty());
    summary.backuped_pages = sorted_vector(state_.backuped());
    summary.evicted_pages = sorted_vector(state_.evicted());
    summary.locked_pages = sorted_vector(state_.locked());
    summary.pending_writeback_pages = sorted_vector(state_.pending_writeback());
    summary.prefetch_planned_pages = sorted_vector(state_.prefetch_planned());
    summary.prefetch_ready_pages = sorted_vector(state_.prefetch_ready());
    summary.prefetch_late_pages = sorted_vector(state_.prefetch_late());
    summary.prefetch_suppressed_pages = sorted_vector(state_.prefetch_suppressed());
    summary.page_hit_counts = state_.hit_counts();

    if (!summary.missing_invariant_facts.empty()) summary.warnings.push_back("Some HiCache target-state inputs are missing token invariant facts.");
    if (summary.dirty_eviction_events > 0) summary.warnings.push_back("Dirty page eviction triggered modeled writeback state transitions.");
    summary.warnings.push_back(
        "HiCacheModule consumes only invariant_state facts; source_actual/timing_observation/oracle_state are ignored for target state.");
    summary.warnings.push_back("HiCacheModule maintains state only; no DAG mutations are applied.");
    return summary;
}

/** @brief 创建临时 HiCacheStateModel 并返回本次运行的 summary。 */
HiCacheSummary apply_hicache_model(DagGraph & graph, const HiCacheConfig & config) {
    HiCacheStateModel model(config);
    return model.run(graph);
}

} // namespace TraceGraph
