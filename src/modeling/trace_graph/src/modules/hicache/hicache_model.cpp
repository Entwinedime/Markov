#include "trace_graph/modules/hicache/hicache_model.hpp"

#include "trace_graph/core/dag_graph.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace TraceGraph {

namespace {

bool contains(const std::string & text, const std::string & needle) { return text.find(needle) != std::string::npos; }

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::vector<std::string> sorted_vector(const std::set<std::string> & values) { return {values.begin(), values.end()}; }

bool is_vector_prefix(const std::vector<std::string> & prefix, const std::vector<std::string> & values) {
    if (prefix.size() > values.size()) return false;
    return std::equal(prefix.begin(), prefix.end(), values.begin());
}

std::vector<std::string> slice_pages(const std::vector<std::string> & pages, size_t begin, size_t end) {
    if (begin >= pages.size() || begin >= end) return {};
    end = std::min(end, pages.size());
    return {pages.begin() + static_cast<long>(begin), pages.begin() + static_cast<long>(end)};
}

std::vector<std::string> suffix_pages(const std::vector<std::string> & pages, size_t begin) { return slice_pages(pages, begin, pages.size()); }

// 这些角色描述的是 source run 中已经发生的 movement。
// 它们不能直接驱动 target state；只能跳过，或在少数 planned completion 场景下作为 completion evidence。
bool is_non_invariant_movement_role(const std::string & role) {
    if (role == "load_back" || role == "init_load_back" || role == "write_backup" || role == "write_storage_schedule" || role == "remove_page" ||
        role == "evict" || role == "l3_hit_query")
        return true;
    // lock/ref 依赖当前 radix tree 的节点拆分和父链路径。page size what-if 下
    // base run 的 inc/dec 次数不是 target timeline 的不变量，只能用于同场景诊断。
    if (role == "lock_ref_inc" || role == "lock_ref_dec") return true;
    // l3_prefetch_enqueue 是 base run 中实际提交给 controller 的预取 enqueue。
    // page size what-if 下 target planned pages 应由 prefetch_schedule + target
    // policy 重新生成；不能消费 base enqueue 上按旧 last_hash 计算出的 target pages。
    if (role == "l3_prefetch_enqueue") return true;
    return contains(role, "to_l1") || contains(role, "to_l2") || contains(role, "to_l3");
}

bool is_prefetch_movement_role(const std::string & role) {
    if (role == "l3_hit_query" || role == "prefetch_loaded_tokens" || role == "l3_prefetch_enqueue") return true;
    return contains(role, "l3_to_l2");
}

bool is_l2_to_l1_fact(const HiCacheFact & fact) {
    return fact.role == "load_back" || fact.role == "init_load_back" || contains(fact.role, "l2_to_l1") || fact.tier_dst == "L1";
}

bool is_l3_to_l2_fact(const HiCacheFact & fact) {
    return fact.role == "l3_to_l2_transfer" || contains(fact.role, "l3_to_l2") || (fact.tier_src == "L3" && fact.tier_dst == "L2");
}

bool is_l1_to_l2_fact(const HiCacheFact & fact) {
    return fact.role == "write_backup" || contains(fact.role, "l1_to_l2") || (fact.tier_src == "L1" && fact.tier_dst == "L2");
}

bool is_l2_to_l3_fact(const HiCacheFact & fact) {
    return fact.role == "write_storage_schedule" || contains(fact.role, "l2_to_l3") || (fact.tier_src == "L2" && fact.tier_dst == "L3");
}

bool is_prefetch_schedule_fact(const HiCacheFact & fact) { return fact.role == "prefetch_schedule" || fact.role == "l3_prefetch_enqueue"; }

bool should_skip_non_invariant_movement(const HiCacheConfig & config, const HiCacheFact & fact) {
    // HiCache state prediction 只能消费不变量。base run 已发生的 movement
    // 不是 target 配置下的答案；即使 target 与 source 同配置，也不能作为
    // target-state answer 直接驱动 state。
    if (is_prefetch_movement_role(fact.role) && contains(fact.role, "l3_to_l2")) {
        if (config.write_policy == "write_back" && !config.write_back_prefetch_transfer_credit) return true;
        // 显式 prefetch target 的 ready pages 仍需要 transfer completion
        // evidence。L3->L2 transfer 不能像普通 movement 一样全部跳过；
        // 是否真正更新状态由 HiCacheState 根据 planned pages 再判断。
        if ((config.prefetch_policy == "best_effort" || config.prefetch_policy == "timeout" || config.prefetch_policy == "wait_complete") &&
            contains(fact.role, "l3_to_l2"))
            return false;
        return true;
    }
    if (is_non_invariant_movement_role(fact.role) || is_l1_to_l2_fact(fact) || is_l2_to_l3_fact(fact) || fact.role == "evict") return true;
    return false;
}

std::string join_set(const std::set<std::string> & values) {
    std::ostringstream os;
    bool first = true;
    for (const auto & value : values) {
        if (!first) os << ",";
        first = false;
        os << value;
    }
    return os.str();
}

std::string join_count_map(const std::unordered_map<std::string, uint64_t> & values) {
    std::map<std::string, uint64_t> ordered(values.begin(), values.end());
    std::ostringstream os;
    bool first = true;
    for (const auto & [key, count] : ordered) {
        if (!first) os << ",";
        first = false;
        os << key << ":" << count;
    }
    return os.str();
}

} // namespace

std::string HiCacheState::digest() const {
    std::ostringstream os;
    os << "l1=" << join_set(l1_) << ";l2=" << join_set(l2_) << ";l3=" << join_set(l3_) << ";dirty=" << join_set(dirty_) << ";backuped=" << join_set(backuped_)
       << ";evicted=" << join_set(evicted_) << ";prefetch_planned=" << join_set(prefetch_planned_) << ";prefetch_ready=" << join_set(prefetch_ready_)
       << ";prefetch_late=" << join_set(prefetch_late_) << ";prefetch_suppressed=" << join_set(prefetch_suppressed_) << ";locked=" << join_set(locked_)
       << ";hit_count=" << join_count_map(hit_count_by_scope_page_);
    return os.str();
}

HiCacheState::HiCacheState(HiCacheConfig config) : config_(std::move(config)) {}

std::vector<HiCacheStateTransition> HiCacheState::apply_fact(const HiCacheFact & fact, HiCacheSummary & summary) {
    std::vector<HiCacheStateTransition> transitions;
    if (!fact.radix_removed_pages.empty()) {
        if (target_page_size_mismatch(fact) && !fact.radix_removed_pages_are_target)
            summary.skipped_non_invariant_events++;
        else
            apply_radix_removed_pages(fact, summary, transitions);
    }
    if (fact.role == "lookup") {
        apply_lookup(fact, summary, transitions);
        return transitions;
    }
    if (fact.role == "insert") {
        auto insert_fact = fact;
        const auto radix_path = target_radix_path_for_insert(fact);
        insert_fact.pages = target_insert_pages(fact);
        apply_insert(insert_fact, summary, transitions);
        radix_tree_.insert_path(radix_path.empty() ? insert_fact.pages : radix_path);
        return transitions;
    }
    if (is_l2_to_l1_fact(fact)) {
        apply_load_to_l1(fact, summary, transitions);
        return transitions;
    }
    if (is_prefetch_schedule_fact(fact)) {
        const auto pages = fact.role == "prefetch_schedule" ? target_prefetch_schedule_pages(fact) : fact.pages;
        remember_prefetch_schedule(fact, pages);
        for (const auto & page : pages) { mark_prefetch_planned(fact, summary, transitions, page); }
        return transitions;
    }
    if (fact.role == "prefetch_progress") {
        apply_prefetch_progress(fact, summary, transitions);
        return transitions;
    }
    if (fact.role == "l3_hit_query") {
        summary.skipped_non_invariant_events++;
        return transitions;
    }
    if (is_l3_to_l2_fact(fact)) {
        apply_l3_to_l2(fact, summary, transitions, contains(fact.role, "prefetch") || fact.direction == "prefetch");
        return transitions;
    }
    if (is_l1_to_l2_fact(fact)) {
        if (config_.write_policy == "write_back") return transitions;
        apply_write_to_l2(fact, summary, transitions);
        return transitions;
    }
    if (is_l2_to_l3_fact(fact)) {
        if (config_.write_policy == "write_back") return transitions;
        apply_write_to_l3(fact, summary, transitions);
        return transitions;
    }
    if (fact.role == "evict") {
        apply_evict(fact, summary, transitions);
        return transitions;
    }
    if (fact.role == "evict_summary") {
        apply_policy_evict(fact, summary, transitions);
        return transitions;
    }
    if (fact.role == "remove_page") {
        apply_remove_page(fact, summary, transitions);
        return transitions;
    }
    if (fact.role == "lock_ref_inc") {
        apply_lock_ref(fact, summary, transitions, true);
        return transitions;
    }
    if (fact.role == "lock_ref_dec") {
        apply_lock_ref(fact, summary, transitions, false);
        return transitions;
    }
    apply_generic_tier_move(fact, summary, transitions);
    return transitions;
}

std::vector<HiCacheStateTransition> HiCacheState::finalize(HiCacheSummary & summary) {
    std::vector<HiCacheStateTransition> transitions;
    finalize_prefetch_policy(summary, transitions);
    return transitions;
}

std::set<std::string> * HiCacheState::tier_set(const std::string & tier) {
    if (tier == "L1") return &l1_;
    if (tier == "L2") return &l2_;
    if (tier == "L3") return &l3_;
    return nullptr;
}

const std::set<std::string> * HiCacheState::tier_set(const std::string & tier) const {
    if (tier == "L1") return &l1_;
    if (tier == "L2") return &l2_;
    if (tier == "L3") return &l3_;
    return nullptr;
}

std::vector<std::string> * HiCacheState::touch_order_for_tier(const std::string & tier) {
    if (tier == "L1") return &l1_touch_order_;
    if (tier == "L2") return &l2_touch_order_;
    return nullptr;
}

uint64_t HiCacheState::capacity_for_tier(const std::string & tier) const {
    if (tier == "L1") return config_.l1_capacity_pages;
    if (tier == "L2") return config_.l2_capacity_pages;
    return 0;
}

bool HiCacheState::target_page_size_mismatch(const HiCacheFact & fact) const {
    return config_.page_size > 0 && fact.page_size > 0 && config_.page_size != fact.page_size;
}

bool HiCacheState::target_capacity_configured() const { return config_.l1_capacity_pages > 0 || config_.l2_capacity_pages > 0; }

bool HiCacheState::target_load_model_enabled(const HiCacheFact & fact) const { return target_page_size_mismatch(fact) || target_capacity_configured(); }

void HiCacheState::apply_lookup(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    if (fact.pages.empty()) return;
    last_lookup_pages_ = fact.pages;
    last_lookup_pages_by_scope_[fact.cache_scope] = fact.pages;
    if (!fact.request_id.empty()) pending_lookup_pages_by_request_[scoped_request_key(fact)] = fact.pages;
    for (const auto & page : fact.pages) {
        if (l1_.count(page) > 0) {
            touch_page("L1", page);
            if (l2_.count(page) > 0) touch_page("L2", page);
            continue;
        }
        // target prediction 会跳过 non-invariant movement；lookup 必须根据目标
        // resident set 自己推导命中后的状态变化。
        if (!target_load_model_enabled(fact)) continue;
        if (l2_.count(page) > 0) {
            touch_page("L2", page);
            add_resident(fact, summary, transitions, "L1", page);
            continue;
        }
        if (l3_.count(page) > 0) {
            add_resident(fact, summary, transitions, "L2", page);
            add_resident(fact, summary, transitions, "L1", page);
            if (config_.prefetch_policy == "wait_complete" && prefetch_planned_.count(page) > 0) mark_prefetch_ready(fact, summary, transitions, page);
        }
    }
    enforce_capacity(fact, summary, transitions, "L2");
    enforce_capacity(fact, summary, transitions, "L1");
}

std::vector<std::string> HiCacheState::target_radix_path_for_insert(const HiCacheFact & fact) const {
    auto lookup_it = fact.request_id.empty() ? pending_lookup_pages_by_request_.end() : pending_lookup_pages_by_request_.find(scoped_request_key(fact));
    auto scope_lookup_it = last_lookup_pages_by_scope_.find(fact.cache_scope);
    const auto & fallback_lookup_pages = scope_lookup_it == last_lookup_pages_by_scope_.end() ? last_lookup_pages_ : scope_lookup_it->second;
    const auto & lookup_pages = lookup_it == pending_lookup_pages_by_request_.end() ? fallback_lookup_pages : lookup_it->second;

    if (target_page_size_mismatch(fact)) {
        if (!lookup_pages.empty()) return lookup_pages;
        if (!fact.target_pages.empty()) return fact.target_pages;
        return fact.pages;
    }
    if (!fact.source_pages.empty()) return fact.source_pages;
    if (!lookup_pages.empty() && is_vector_prefix(fact.pages, lookup_pages)) return lookup_pages;
    return fact.pages;
}

std::vector<std::string> HiCacheState::target_insert_pages(const HiCacheFact & fact) const {
    const auto radix_path = target_radix_path_for_insert(fact);
    if (radix_path.empty()) return fact.pages;
    size_t known_prefix_pages = 0;
    const bool has_request_lookup =
        !fact.request_id.empty() && pending_lookup_pages_by_request_.find(scoped_request_key(fact)) != pending_lookup_pages_by_request_.end();
    if (has_request_lookup) {
        while (known_prefix_pages < radix_path.size() && target_prefix_page_known(radix_path[known_prefix_pages])) ++known_prefix_pages;
    }
    const auto prefix_pages = std::max(radix_tree_.longest_prefix_pages(radix_path), known_prefix_pages);
    if (prefix_pages >= radix_path.size()) return {};
    return suffix_pages(radix_path, prefix_pages);
}

bool HiCacheState::target_prefix_page_known(const std::string & page) const {
    return !page.empty() && (radix_tree_.contains_page(page) || l1_.count(page) > 0 || l2_.count(page) > 0 || backuped_.count(page) > 0);
}

void HiCacheState::apply_radix_removed_pages(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    std::unordered_set<std::string> seen;
    std::vector<std::string> removed_pages;
    auto erase_from_order = [](std::vector<std::string> & order, const std::string & page) {
        order.erase(std::remove(order.begin(), order.end(), page), order.end());
    };
    auto erase_scoped_page = [](std::unordered_map<std::string, uint64_t> & counts, const std::string & page) {
        const auto suffix = ":" + page;
        for (auto it = counts.begin(); it != counts.end();) {
            if (it->first.size() >= suffix.size() && it->first.compare(it->first.size() - suffix.size(), suffix.size(), suffix) == 0)
                it = counts.erase(it);
            else
                ++it;
        }
    };

    for (const auto & page : fact.radix_removed_pages) {
        if (page.empty() || !seen.insert(page).second) continue;
        removed_pages.push_back(page);
        remove_resident(fact, summary, transitions, "L1", page);
        remove_resident(fact, summary, transitions, "L2", page);
        clear_dirty(fact, summary, transitions, page);
        clear_backuped(fact, summary, transitions, page);
        clear_evicted(fact, summary, transitions, page);
        clear_locked(fact, summary, transitions, page);

        erase_from_order(l1_touch_order_, page);
        erase_from_order(l2_touch_order_, page);
        erase_scoped_page(hit_count_by_scope_page_, page);
        erase_scoped_page(lock_count_by_scope_page_, page);
    }
    radix_tree_.remove_pages(removed_pages);
}

void HiCacheState::apply_insert(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    auto dst = fact.tier_dst.empty() ? "L1" : fact.tier_dst;
    if (target_capacity_configured() && !fact.pages.empty()) {
        if (dst == "L1") enforce_insert_capacity(fact, summary, transitions, "L1", fact.pages);
        if (config_.write_policy == "write_through") enforce_insert_capacity(fact, summary, transitions, "L2", fact.pages);
    }
    for (const auto & page : fact.pages) {
        add_resident(fact, summary, transitions, dst, page);
        if (config_.write_policy == "write_through") {
            add_resident(fact, summary, transitions, "L2", page);
            // 显式 write-through what-if 会跳过 base trace 中观测到的
            // write_storage movement，因此 insert 必须在目标状态中同步补出
            // storage readable set；否则后续 lookup 无法从 L3 推导 target load。
            add_resident(fact, summary, transitions, "L3", page);
            mark_backuped(fact, summary, transitions, page);
            clear_dirty(fact, summary, transitions, page);
        }
        else {
            const bool dirty_insert_overwrites_backup =
                fact.dirty && !fact.backuped && config_.write_policy != "write_through" && config_.write_policy != "write_through_selective";
            if (dirty_insert_overwrites_backup && backuped_.count(page) > 0) {
                remove_resident(fact, summary, transitions, "L2", page);
                clear_backuped(fact, summary, transitions, page);
            }
            if (fact.dirty && !backuped_.count(page)) mark_dirty(fact, summary, transitions, page);
            if (fact.backuped) mark_backuped(fact, summary, transitions, page);
        }
    }
    apply_write_policy_hit_counts(fact, summary, transitions);
    enforce_capacity(fact, summary, transitions, dst);
    enforce_capacity(fact, summary, transitions, "L2");
}

uint64_t HiCacheState::missing_resident_page_count(const std::string & tier, const std::vector<std::string> & pages) const {
    const auto * resident = tier_set(tier);
    if (!resident) return 0;
    uint64_t count = 0;
    for (const auto & page : pages) {
        if (!resident->count(page)) ++count;
    }
    return count;
}

uint64_t HiCacheState::pages_to_free_for_insert(const std::string & tier, const std::vector<std::string> & pages) const {
    const auto * resident = tier_set(tier);
    const uint64_t capacity = capacity_for_tier(tier);
    if (!resident || capacity == 0) return 0;
    const uint64_t incoming = missing_resident_page_count(tier, pages);
    if (resident->size() + incoming <= capacity) return 0;
    return static_cast<uint64_t>(resident->size() + incoming - capacity);
}

void HiCacheState::enforce_insert_capacity(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                           const std::string & tier, const std::vector<std::string> & pages) {
    evict_lru_pages(fact, summary, transitions, tier, pages_to_free_for_insert(tier, pages));
}

uint64_t HiCacheState::target_write_through_threshold() const {
    if (config_.write_through_threshold > 0) return config_.write_through_threshold;
    if (config_.write_policy == "write_through") return 1;
    if (config_.write_policy == "write_through_selective") return 2;
    return 0;
}

bool HiCacheState::target_write_count_enabled() const { return config_.write_policy == "write_through" || config_.write_policy == "write_through_selective"; }

std::string HiCacheState::scoped_request_key(const HiCacheFact & fact) const {
    if (fact.request_id.empty()) return "";
    return (fact.cache_scope.empty() ? std::string("-1") : fact.cache_scope) + ":" + fact.request_id;
}

std::string HiCacheState::scoped_page_key(const HiCacheFact & fact, const std::string & page) const {
    return (fact.cache_scope.empty() ? std::string("-1") : fact.cache_scope) + ":" + page;
}

bool HiCacheState::page_locked_in_any_scope(const std::string & page) const {
    const auto suffix = ":" + page;
    for (const auto & [key, count] : lock_count_by_scope_page_) {
        if (count > 0 && key.size() >= suffix.size() && key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0) return true;
    }
    return false;
}

std::vector<std::string> HiCacheState::write_policy_hit_pages_for_insert(const HiCacheFact & fact) const {
    std::vector<std::string> pages;
    if (!fact.request_id.empty()) {
        auto lookup_it = pending_lookup_pages_by_request_.find(scoped_request_key(fact));
        if (lookup_it != pending_lookup_pages_by_request_.end()) pages = lookup_it->second;
    }
    if (pages.empty()) {
        auto scope_lookup_it = last_lookup_pages_by_scope_.find(fact.cache_scope);
        if (scope_lookup_it != last_lookup_pages_by_scope_.end()) pages = scope_lookup_it->second;
    }
    if (pages.empty()) pages = fact.pages;

    std::set<std::string> seen(pages.begin(), pages.end());
    for (const auto & page : fact.pages) {
        if (!page.empty() && seen.insert(page).second) pages.push_back(page);
    }
    return pages;
}

bool HiCacheState::write_policy_prefix_backup_ready(const std::vector<std::string> & pages, size_t index) const {
    for (size_t prefix_index = 0; prefix_index < index; ++prefix_index) {
        const auto & prefix_page = pages[prefix_index];
        if (!prefix_page.empty() && backuped_.count(prefix_page) == 0) return false;
    }
    return true;
}

void HiCacheState::apply_write_policy_hit_counts(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    if (!target_write_count_enabled() || fact.chunked) return;
    const uint64_t threshold = target_write_through_threshold();
    if (threshold == 0) return;

    const auto pages = write_policy_hit_pages_for_insert(fact);
    std::set<std::string> counted;
    for (size_t index = 0; index < pages.size(); ++index) {
        const auto & page = pages[index];
        if (page.empty() || !counted.insert(page).second) continue;
        if (l1_.count(page) == 0 && !radix_tree_.contains_page(page)) continue;

        const auto before = transition_state_digest();
        const auto scoped_key = scoped_page_key(fact, page);
        hit_count_by_scope_page_[scoped_key]++;
        record_transition(fact, summary, transitions, "increment_hit_count", "", page, before);

        if (backuped_.count(page) > 0) continue;
        if (hit_count_by_scope_page_[scoped_key] < threshold) continue;
        if (!write_policy_prefix_backup_ready(pages, index)) continue;
        backup_page_for_write_policy(fact, summary, transitions, page);
    }
}

void HiCacheState::backup_page_for_write_policy(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                                const std::string & page) {
    add_resident(fact, summary, transitions, "L1", page);
    add_resident(fact, summary, transitions, "L2", page);
    // target prediction 会跳过 base trace 的 write_storage movement；这里把
    // 达到阈值的 page 同步标成 storage readable，后续 L3->L2 load 才有状态来源。
    add_resident(fact, summary, transitions, "L3", page);
    mark_backuped(fact, summary, transitions, page);
    clear_dirty(fact, summary, transitions, page);
    enforce_capacity(fact, summary, transitions, "L2");
}

void HiCacheState::apply_load_to_l1(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    if (target_capacity_configured() && is_l2_to_l1_fact(fact)) {
        summary.skipped_non_invariant_events++;
        return;
    }
    auto src = fact.tier_src.empty() ? "L2" : fact.tier_src;
    if (target_capacity_configured() && config_.l1_capacity_pages > 0 && l1_.size() + fact.pages.size() > config_.l1_capacity_pages)
        evict_lru_pages(fact, summary, transitions, "L1", fact.pages.size());
    for (const auto & page : fact.pages) {
        add_resident(fact, summary, transitions, src, page);
        add_resident(fact, summary, transitions, "L1", page);
    }
    enforce_capacity(fact, summary, transitions, src);
    enforce_capacity(fact, summary, transitions, "L1");
}

void HiCacheState::apply_l3_to_l2(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, bool ready) {
    const auto pages = target_prefetch_completion_pages(fact);
    bool planned_prefetch_transfer = false;
    for (const auto & page : pages) {
        if (prefetch_planned_.count(page) > 0) {
            planned_prefetch_transfer = true;
            break;
        }
    }
    const bool best_effort_credit = config_.prefetch_policy == "best_effort" && planned_prefetch_transfer;
    const bool timeout_credit = config_.prefetch_policy == "timeout" && planned_prefetch_transfer;
    const bool wait_complete_credit = config_.prefetch_policy == "wait_complete" && planned_prefetch_transfer;
    if (!best_effort_credit && !timeout_credit && !wait_complete_credit) {
        summary.skipped_non_invariant_events++;
        return;
    }
    size_t credited_pages = pages.size();
    if (fact.prefetch_ready_page_count > 0) credited_pages = std::min<size_t>(credited_pages, static_cast<size_t>(fact.prefetch_ready_page_count));
    size_t resident_pages = credited_pages;
    const bool prefetch_transfer = ready || planned_prefetch_transfer;
    const auto request_key = fact.request_id;
    if (prefetch_transfer && !request_key.empty()) {
        if (prefetch_transfer_resident_credited_requests_.count(request_key) > 0)
            resident_pages = 0;
        else if (resident_pages > 0)
            prefetch_transfer_resident_credited_requests_.insert(request_key);
    }
    for (size_t index = 0; index < credited_pages; ++index) {
        const auto & page = pages[index];
        if (index < resident_pages) {
            add_resident(fact, summary, transitions, "L3", page);
            add_resident(fact, summary, transitions, "L2", page);
        }
        if (ready || prefetch_planned_.count(page) > 0) mark_prefetch_ready(fact, summary, transitions, page);
    }
    enforce_capacity(fact, summary, transitions, "L2");
}

void HiCacheState::apply_prefetch_progress(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    const bool terminal_empty_progress = fact.pages.empty() && fact.prefetch_check_available && fact.prefetch_check_return && !fact.prefetch_has_ongoing;
    const std::string effective_prefetch_policy = config_.prefetch_policy;
    const bool has_progress_payload = fact.prefetch_progress_evidence;
    if (!fact.request_id.empty() && terminal_empty_progress && effective_prefetch_policy == "best_effort") {
        auto pending_it = pending_prefetch_pages_by_request_.find(scoped_request_key(fact));
        if (pending_it != pending_prefetch_pages_by_request_.end()) {
            for (const auto & page : pending_it->second) mark_prefetch_suppressed(fact, summary, transitions, page);
        }
    }
    if (!fact.request_id.empty() && terminated_prefetch_requests_.count(scoped_request_key(fact)) > 0) return;

    const auto pages = prefetch_pages_for_fact(fact);
    const bool has_operation_progress_pages =
        !fact.pages.empty() || (!fact.request_id.empty() &&
                                latest_prefetch_progress_pages_by_request_.find(scoped_request_key(fact)) != latest_prefetch_progress_pages_by_request_.end());
    if (!fact.request_id.empty() && !fact.pages.empty()) latest_prefetch_progress_pages_by_request_[scoped_request_key(fact)] = fact.pages;
    if (pages.empty()) return;

    if (!fact.request_id.empty()) remember_prefetch_pages(fact, pages);

    if (!has_operation_progress_pages && fact.prefetch_has_ongoing && !fact.prefetch_check_return) return;

    // 没有显式 progress payload 时，只保留 pending/终止语义，不把 progress
    // 观测反推出 ready page。否则纯 prefetch_done 事件会把 page64 strict
    // prediction 里的 ready 集合过度膨胀。
    const bool progress_ready_credit_allowed = config_.write_policy != "write_back";
    const auto ready_count = has_progress_payload && progress_ready_credit_allowed ? std::min<uint64_t>(fact.prefetch_ready_page_count, pages.size()) : 0;
    if (ready_count > 0) {
        for (size_t index = 0; index < static_cast<size_t>(ready_count); ++index) {
            const auto & page = pages[index];
            add_resident(fact, summary, transitions, "L3", page);
            add_resident(fact, summary, transitions, "L2", page);
            mark_prefetch_ready(fact, summary, transitions, page);
        }
        enforce_capacity(fact, summary, transitions, "L2");
    }

    if (!should_terminate_prefetch_at_progress(fact, pages, ready_count)) return;
    if (!fact.request_id.empty()) terminated_prefetch_requests_.insert(scoped_request_key(fact));
    if (config_.prefetch_policy == "wait_complete") return;

    if (!has_operation_progress_pages && !fact.prefetch_has_ongoing && ready_count == 0) {
        for (const auto & page : pages) mark_prefetch_suppressed(fact, summary, transitions, page);
        return;
    }
    for (size_t index = static_cast<size_t>(ready_count); index < pages.size(); ++index) { mark_prefetch_late(fact, summary, transitions, pages[index]); }
}

void HiCacheState::apply_write_to_l2(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    for (const auto & page : fact.pages) {
        add_resident(fact, summary, transitions, "L1", page);
        add_resident(fact, summary, transitions, "L2", page);
        mark_backuped(fact, summary, transitions, page);
        clear_dirty(fact, summary, transitions, page);
    }
    enforce_capacity(fact, summary, transitions, "L2");
}

void HiCacheState::apply_write_to_l3(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    for (const auto & page : fact.pages) {
        add_resident(fact, summary, transitions, "L2", page);
        add_resident(fact, summary, transitions, "L3", page);
        mark_backuped(fact, summary, transitions, page);
        clear_dirty(fact, summary, transitions, page);
    }
    enforce_capacity(fact, summary, transitions, "L2");
}

void HiCacheState::apply_evict(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    for (const auto & page : fact.pages) {
        if (dirty_.count(page)) {
            summary.dirty_eviction_events++;
            flush_dirty_page_to_host(fact, summary, transitions, page);
        }
        if (!fact.tier_src.empty())
            remove_resident(fact, summary, transitions, fact.tier_src, page);
        else if (l1_.count(page))
            remove_resident(fact, summary, transitions, "L1", page);
        else if (l2_.count(page))
            remove_resident(fact, summary, transitions, "L2", page);
        else if (l3_.count(page))
            remove_resident(fact, summary, transitions, "L3", page);
        mark_evicted(fact, summary, transitions, page);
    }
}

void HiCacheState::apply_policy_evict(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    // evict_summary 只消费 count-only 事实，不消费返回的 page identity。
    // start 事件没有返回的 eviction 结果，避免 start/end 双触发。
    if (fact.is_start) return;
    const uint64_t target_page_size = config_.page_size > 0 ? config_.page_size : fact.page_size;
    if (target_page_size == 0 || fact.requested_tokens == 0) return;

    const std::string dedupe_key = std::to_string(fact.requested_tokens) + ":" + std::to_string(fact.evicted_tokens);
    if (dedupe_key == last_policy_evict_key_ && fact.ts >= last_policy_evict_ts_ && fact.ts - last_policy_evict_ts_ < 1000000) return;
    last_policy_evict_key_ = dedupe_key;
    last_policy_evict_ts_ = fact.ts;

    const uint64_t pages_to_free = (fact.requested_tokens + target_page_size - 1) / target_page_size;
    evict_lru_pages(fact, summary, transitions, "L1", pages_to_free);
}

void HiCacheState::apply_remove_page(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    if (target_capacity_configured()) {
        summary.skipped_non_invariant_events++;
        return;
    }
    for (const auto & page : fact.pages) {
        const auto tier = fact.tier_src.empty() ? "L1" : fact.tier_src;
        if (tier == "L1" && config_.write_policy == "write_back" && dirty_.count(page)) {
            summary.dirty_eviction_events++;
            flush_dirty_page_to_host(fact, summary, transitions, page);
        }
        remove_resident(fact, summary, transitions, tier, page);
        if (tier == "L2") {
            clear_backuped(fact, summary, transitions, page);
            clear_evicted(fact, summary, transitions, page);
        }
        else if (tier == "L1") {
            mark_evicted(fact, summary, transitions, page);
        }
    }
}

void HiCacheState::apply_lock_ref(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, bool increment) {
    summary.lock_state_events++;
    for (const auto & page : fact.pages) {
        const auto key = scoped_page_key(fact, page);
        if (increment) {
            lock_count_by_scope_page_[key]++;
            mark_locked(fact, summary, transitions, page);
            continue;
        }
        auto it = lock_count_by_scope_page_.find(key);
        if (it != lock_count_by_scope_page_.end() && it->second > 0) {
            it->second--;
            if (it->second == 0) {
                lock_count_by_scope_page_.erase(it);
                if (!page_locked_in_any_scope(page)) clear_locked(fact, summary, transitions, page);
            }
        }
        else {
            if (!page_locked_in_any_scope(page)) clear_locked(fact, summary, transitions, page);
        }
    }
}

void HiCacheState::apply_generic_tier_move(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    for (const auto & page : fact.pages) {
        if (!fact.tier_src.empty()) add_resident(fact, summary, transitions, fact.tier_src, page);
        if (!fact.tier_dst.empty()) add_resident(fact, summary, transitions, fact.tier_dst, page);
        if (fact.direction == "write" || fact.tier_dst == "L3") {
            mark_backuped(fact, summary, transitions, page);
            clear_dirty(fact, summary, transitions, page);
        }
        if (fact.direction == "evict" && !fact.tier_src.empty()) remove_resident(fact, summary, transitions, fact.tier_src, page);
    }
    if (!fact.tier_src.empty()) enforce_capacity(fact, summary, transitions, fact.tier_src);
    if (!fact.tier_dst.empty()) enforce_capacity(fact, summary, transitions, fact.tier_dst);
}

void HiCacheState::add_resident(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, const std::string & tier,
                                const std::string & page) {
    auto * pages = tier_set(tier);
    if (!pages) return;
    auto before = transition_state_digest();
    touch_page(tier, page);
    if (pages->insert(page).second) {
        evicted_.erase(page);
        record_transition(fact, summary, transitions, "add_" + lower(tier) + "_resident", tier, page, before);
    }
    if (tier == "L2") mark_backuped(fact, summary, transitions, page);
}

void HiCacheState::remove_resident(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                   const std::string & tier, const std::string & page) {
    auto * pages = tier_set(tier);
    if (!pages) return;
    auto before = transition_state_digest();
    if (pages->erase(page) > 0) {
        record_transition(fact, summary, transitions, "remove_" + lower(tier) + "_resident", tier, page, before);
        if (tier == "L2") clear_backuped(fact, summary, transitions, page);
    }
}

void HiCacheState::mark_dirty(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, const std::string & page) {
    auto before = transition_state_digest();
    if (dirty_.insert(page).second) record_transition(fact, summary, transitions, "mark_dirty", "", page, before);
}

void HiCacheState::clear_dirty(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                               const std::string & page) {
    auto before = transition_state_digest();
    if (dirty_.erase(page) > 0) record_transition(fact, summary, transitions, "clear_dirty", "", page, before);
}

void HiCacheState::mark_backuped(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                 const std::string & page) {
    auto before = transition_state_digest();
    if (backuped_.insert(page).second) record_transition(fact, summary, transitions, "mark_backuped", "", page, before);
    // SGLang 的 TreeNode.backuped 表示 host_value 已存在；一旦备份成立，
    // 该页不应继续留在 dirty 集合中。
    clear_dirty(fact, summary, transitions, page);
}

void HiCacheState::clear_backuped(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                  const std::string & page) {
    auto before = transition_state_digest();
    if (backuped_.erase(page) > 0) record_transition(fact, summary, transitions, "clear_backuped", "", page, before);
}

void HiCacheState::mark_evicted(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                const std::string & page) {
    auto before = transition_state_digest();
    if (evicted_.insert(page).second) record_transition(fact, summary, transitions, "mark_evicted", "", page, before);
}

void HiCacheState::clear_evicted(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                 const std::string & page) {
    auto before = transition_state_digest();
    if (evicted_.erase(page) > 0) record_transition(fact, summary, transitions, "clear_evicted", "", page, before);
}

void HiCacheState::mark_locked(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                               const std::string & page) {
    auto before = transition_state_digest();
    if (locked_.insert(page).second) record_transition(fact, summary, transitions, "mark_locked", "", page, before);
}

void HiCacheState::clear_locked(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                const std::string & page) {
    auto before = transition_state_digest();
    if (locked_.erase(page) > 0) record_transition(fact, summary, transitions, "clear_locked", "", page, before);
}

void HiCacheState::mark_prefetch_planned(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                         const std::string & page) {
    auto before = transition_state_digest();
    if (prefetch_planned_.insert(page).second) record_transition(fact, summary, transitions, "mark_prefetch_planned", "", page, before);
}

void HiCacheState::mark_prefetch_ready(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                       const std::string & page) {
    auto before = transition_state_digest();
    if (prefetch_ready_.insert(page).second) record_transition(fact, summary, transitions, "mark_prefetch_ready", "", page, before);
    prefetch_late_.erase(page);
    prefetch_suppressed_.erase(page);
}

void HiCacheState::mark_prefetch_late(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                      const std::string & page) {
    if (prefetch_ready_.count(page) > 0) return;
    auto before = transition_state_digest();
    if (prefetch_late_.insert(page).second) record_transition(fact, summary, transitions, "mark_prefetch_late", "", page, before);
}

void HiCacheState::mark_prefetch_suppressed(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                            const std::string & page) {
    if (prefetch_ready_.count(page) > 0) return;
    auto before = transition_state_digest();
    if (prefetch_suppressed_.insert(page).second) record_transition(fact, summary, transitions, "mark_prefetch_suppressed", "", page, before);
}

void HiCacheState::remember_prefetch_pages(const HiCacheFact & fact, const std::vector<std::string> & pages) {
    if (fact.request_id.empty() || pages.empty()) return;
    auto & remembered = pending_prefetch_pages_by_request_[scoped_request_key(fact)];
    std::unordered_set<std::string> seen(remembered.begin(), remembered.end());
    for (const auto & page : pages) {
        if (!page.empty() && seen.insert(page).second) remembered.push_back(page);
    }
}

void HiCacheState::remember_prefetch_schedule(const HiCacheFact & fact, const std::vector<std::string> & pages) {
    remember_prefetch_pages(fact, pages);
    if (fact.request_id.empty()) return;
    const auto key = scoped_request_key(fact);
    prefetch_schedule_ts_by_request_[key] = fact.ts;
    latest_prefetch_schedule_pages_by_request_[key] = pages;
    if (fact.role == "prefetch_schedule" && target_page_size_mismatch(fact) && !fact.source_pages.empty()) {
        auto & source_page_count = prefetch_schedule_source_page_count_by_request_[key];
        source_page_count = std::max<uint64_t>(source_page_count, fact.source_pages.size());
    }
}

std::vector<std::string> HiCacheState::target_prefetch_schedule_pages(const HiCacheFact & fact) const {
    if (!target_page_size_mismatch(fact)) return fact.pages;
    if (config_.prefetch_policy == "best_effort" && !fact.pages.empty()) return fact.pages;
    if (fact.request_id.empty() || fact.new_input_tokens == 0 || config_.page_size == 0) return fact.pages;

    const auto lookup_it = pending_lookup_pages_by_request_.find(scoped_request_key(fact));
    if (lookup_it == pending_lookup_pages_by_request_.end() || lookup_it->second.empty()) return fact.pages;

    // page size what-if 下，prefetch_from_storage 的 prefix_keys 可能为空；
    // probe 只能生成没有 parent hash 的 target_page_identity。lookup 已经
    // 暴露了同 request 在目标 page size 下的完整 path，这里按目标 page
    // size 从 path 尾部截出 new_input_tokens 对应的完整 suffix pages。
    const size_t suffix_pages = static_cast<size_t>(fact.new_input_tokens / config_.page_size);
    if (suffix_pages == 0 || lookup_it->second.size() < suffix_pages) return fact.pages;
    return {lookup_it->second.end() - static_cast<long>(suffix_pages), lookup_it->second.end()};
}

std::vector<std::string> HiCacheState::target_prefetch_completion_pages(const HiCacheFact & fact) const {
    if (!target_page_size_mismatch(fact) || fact.request_id.empty()) return fact.pages;
    if (config_.prefetch_policy != "best_effort" && config_.prefetch_policy != "timeout" && config_.prefetch_policy != "wait_complete") return fact.pages;

    const auto key = scoped_request_key(fact);
    const auto schedule_it = latest_prefetch_schedule_pages_by_request_.find(key);
    if (schedule_it == latest_prefetch_schedule_pages_by_request_.end() || schedule_it->second.empty()) return fact.pages;

    const auto source_count_it = prefetch_schedule_source_page_count_by_request_.find(key);
    if (source_count_it == prefetch_schedule_source_page_count_by_request_.end() || source_count_it->second == 0) return fact.pages;
    if (fact.source_pages.size() < source_count_it->second) return fact.pages;

    // page size what-if 下，base transfer 的 target_page_identity 只覆盖
    // base operation token 数能整除出的 target pages，也可能因为 base
    // prefetch 的 last_hash / parent context 不同而和 target schedule
    // identity 不完全一致。只要 source pages 已覆盖该 request 的 schedule
    // source pages，completion credit 应归到目标配置计划出的 suffix pages。
    return schedule_it->second;
}

std::vector<std::string> HiCacheState::prefetch_pages_for_fact(const HiCacheFact & fact) const {
    if (!fact.pages.empty()) return fact.pages;
    if (fact.request_id.empty()) return {};
    const auto key = scoped_request_key(fact);
    auto progress_it = latest_prefetch_progress_pages_by_request_.find(key);
    if (progress_it != latest_prefetch_progress_pages_by_request_.end() && !progress_it->second.empty()) return progress_it->second;
    auto pending_it = pending_prefetch_pages_by_request_.find(key);
    if (pending_it != pending_prefetch_pages_by_request_.end()) return pending_it->second;
    return {};
}

void HiCacheState::finalize_prefetch_policy(HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    HiCacheFact fact;
    fact.role = "prefetch_finalize";
    fact.event_name = "hicache_prefetch_finalize";
    if (config_.prefetch_policy == "best_effort" || config_.prefetch_policy == "wait_complete") {
        // best_effort 在 run 结束时不会再等待未完成预取；wait_complete
        // 在真实 trace 中也可能只产生 planned 而没有可完成的 storage
        // operation。对当前 run 来说，这些 planned 但未 ready 的 page
        // 应作为 suppressed final state 暴露，方便和 oracle 对齐。
        for (const auto & page : prefetch_planned_) {
            if (prefetch_ready_.count(page) > 0) continue;
            if (prefetch_suppressed_.count(page) > 0) continue;
            mark_prefetch_suppressed(fact, summary, transitions, page);
        }
        return;
    }
    if (config_.prefetch_policy != "timeout") return;

    // timeout 不能把所有 planned page 都在尾部强行归为 suppressed。
    // 普通 timeout run 可能只暴露 planned/ready，不暴露 timeout 终止；
    // 只有已经由 progress evidence 证明终止的 request，才补齐其未 ready
    // page 的 suppressed final state。
    for (const auto & request_key : terminated_prefetch_requests_) {
        auto it = pending_prefetch_pages_by_request_.find(request_key);
        if (it == pending_prefetch_pages_by_request_.end()) continue;
        for (const auto & page : it->second) {
            if (prefetch_ready_.count(page) > 0) continue;
            if (prefetch_suppressed_.count(page) > 0) continue;
            mark_prefetch_suppressed(fact, summary, transitions, page);
        }
    }
}

bool HiCacheState::should_terminate_prefetch_at_progress(const HiCacheFact & fact, const std::vector<std::string> & pages, uint64_t ready_count) const {
    const auto policy = config_.prefetch_policy.empty() ? std::string("timeout") : config_.prefetch_policy;
    if (policy == "best_effort") {
        // SGLang best_effort 在第一次 check_prefetch_progress 时允许 terminate。
        // 因此 prediction 不能等 base trace 的 check_return=true。
        return fact.prefetch_progress_evidence || fact.prefetch_check_available;
    }
    if (policy == "wait_complete") { return !pages.empty() && ready_count >= pages.size(); }
    if (policy == "timeout") {
        if (!pages.empty() && ready_count >= pages.size()) return true;
        if (fact.prefetch_check_available && fact.prefetch_check_return && !fact.prefetch_has_ongoing) return true;
        if (prefetch_timeout_reached(fact, pages)) return true;
        // 没有显式 target timeout 参数时保留缺省真实 check 返回语义。
        return !config_.prefetch_timeout_configured && fact.prefetch_check_available && fact.prefetch_check_return;
    }
    return false;
}

bool HiCacheState::prefetch_timeout_reached(const HiCacheFact & fact, const std::vector<std::string> & pages) const {
    if (!config_.prefetch_timeout_configured || fact.request_id.empty()) return false;
    auto schedule_it = prefetch_schedule_ts_by_request_.find(scoped_request_key(fact));
    if (schedule_it == prefetch_schedule_ts_by_request_.end()) return false;
    if (fact.ts < schedule_it->second) return false;

    const uint64_t page_size = config_.page_size > 0 ? config_.page_size : fact.page_size;
    const uint64_t token_count = page_size > 0 ? static_cast<uint64_t>(pages.size()) * page_size : 0;
    double timeout_sec = config_.prefetch_timeout_base_sec + config_.prefetch_timeout_per_ki_token_sec * static_cast<double>(token_count) / 1024.0;
    if (config_.prefetch_timeout_max_sec > 0.0) timeout_sec = std::min(timeout_sec, config_.prefetch_timeout_max_sec);
    if (timeout_sec < 0.0) timeout_sec = 0.0;

    const uint64_t elapsed_us = fact.ts - schedule_it->second;
    const double elapsed_sec = static_cast<double>(elapsed_us) / 1000000.0;
    return elapsed_sec >= timeout_sec;
}

void HiCacheState::touch_page(const std::string & tier, const std::string & page) {
    auto * order = touch_order_for_tier(tier);
    if (!order) return;
    order->erase(std::remove(order->begin(), order->end(), page), order->end());
    order->push_back(page);
}

void HiCacheState::flush_dirty_page_to_host(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                            const std::string & page) {
    add_resident(fact, summary, transitions, "L2", page);
    mark_backuped(fact, summary, transitions, page);
    clear_dirty(fact, summary, transitions, page);
}

std::map<std::string, uint64_t> HiCacheState::page_hit_count_summary() const {
    std::map<std::string, uint64_t> result;
    for (const auto & [scoped_key, count] : hit_count_by_scope_page_) {
        const auto separator = scoped_key.find(':');
        const auto page = separator == std::string::npos ? scoped_key : scoped_key.substr(separator + 1);
        auto & current = result[page];
        current = std::max(current, count);
    }
    return result;
}

void HiCacheState::evict_lru_pages(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                   const std::string & tier, uint64_t page_count) {
    auto * pages = tier_set(tier);
    auto * order = touch_order_for_tier(tier);
    if (!pages || !order || page_count == 0) return;

    uint64_t removed = 0;
    while (removed < page_count && !order->empty()) {
        auto victim = order->front();
        order->erase(order->begin());
        if (!pages->count(victim)) continue;
        auto victims = radix_tree_.leaf_group_for_page(victim);
        if (victims.empty()) victims = {victim};
        for (const auto & page : victims) {
            if (!pages->count(page)) continue;
            order->erase(std::remove(order->begin(), order->end(), page), order->end());
            if (dirty_.count(page)) {
                summary.dirty_eviction_events++;
                flush_dirty_page_to_host(fact, summary, transitions, page);
            }
            remove_resident(fact, summary, transitions, tier, page);
            if (tier == "L2")
                clear_evicted(fact, summary, transitions, page);
            else
                mark_evicted(fact, summary, transitions, page);
            removed++;
        }
    }
}

void HiCacheState::enforce_capacity(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                    const std::string & tier) {
    auto capacity = capacity_for_tier(tier);
    if (capacity == 0) return;
    auto * pages = tier_set(tier);
    auto * order = touch_order_for_tier(tier);
    if (!pages || !order) return;
    while (pages->size() > capacity && !order->empty()) { evict_lru_pages(fact, summary, transitions, tier, pages->size() - capacity); }
}

std::string HiCacheState::transition_state_digest() const { return config_.emit_state_digests ? digest() : std::string{}; }

void HiCacheState::record_transition(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                     const std::string & kind, const std::string & tier, const std::string & page, const std::string & before_digest) {
    summary.state_transition_count++;
    summary.transitions_by_kind[kind]++;
    HiCacheStateTransition transition;
    transition.transition_id = "hicache_transition_" + std::to_string(summary.state_transition_count);
    transition.kind = kind;
    transition.role = fact.role;
    transition.request_id = fact.request_id;
    transition.operation_id = fact.operation_id;
    transition.event_name = fact.event_name;
    transition.cache_scope = fact.cache_scope;
    transition.ts = fact.ts;
    transition.source_event_index = fact.source_event_index;
    transition.tier = tier;
    transition.pages = {page};
    transition.before_state_digest = before_digest;
    transition.after_state_digest = transition_state_digest();
    transitions.push_back(std::move(transition));
}

HiCacheStateModel::HiCacheStateModel(HiCacheConfig config) : config_(std::move(config)), state_(config_) {}

HiCacheSummary HiCacheStateModel::run(DagGraph & graph) {
    HiCacheSummary summary;
    summary.target_config = config_;
    if (!config_.enabled) {
        summary.status = "disabled";
        return summary;
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

    // Python probe 用 X event 表达 start/end 时，merged trace 中同 timestamp
    // 可能出现 end 在 start 前。state model 需要按调用逻辑顺序消费事实，否则
    // prefetch_progress_end 会在 operation progress evidence 之前被误判为 suppressed。
    std::stable_sort(facts.begin(), facts.end(), [](const HiCacheFact & left, const HiCacheFact & right) {
        if (left.ts != right.ts) return left.ts < right.ts;
        if (left.is_start != right.is_start) return left.is_start && !right.is_start;
        return left.source_event_index < right.source_event_index;
    });

    for (auto fact : facts) {
        if (fact.is_start && fact.role != "prefetch_progress") continue;

        summary.processed_hicache_events++;
        summary.processed_events_by_role[fact.role]++;
        if (config_.page_size > 0 && fact.page_size > 0 && config_.page_size != fact.page_size) {
            const auto target_pages_it = fact.target_pages_by_page_size.find(config_.page_size);
            if (target_pages_it != fact.target_pages_by_page_size.end() && !target_pages_it->second.empty()) { fact.target_pages = target_pages_it->second; }
            const auto target_removed_it = fact.target_radix_removed_pages_by_page_size.find(config_.page_size);
            if (target_removed_it != fact.target_radix_removed_pages_by_page_size.end() && !target_removed_it->second.empty()) {
                fact.target_radix_removed_pages = target_removed_it->second;
            }
        }
        const bool target_completion_evidence = contains(fact.role, "l3_to_l2") && !fact.target_pages.empty();
        if (config_.page_size > 0 && fact.page_size > 0 && config_.page_size != fact.page_size) {
            if (!fact.target_radix_removed_pages.empty()) {
                fact.radix_removed_pages = fact.target_radix_removed_pages;
                fact.radix_removed_pages_are_target = true;
            }
            if (!target_completion_evidence && is_non_invariant_movement_role(fact.role)) {
                summary.skipped_non_invariant_events++;
                continue;
            }
            if (!fact.target_pages.empty()) {
                fact.pages = fact.target_pages;
                if (contains(fact.role, "l3_to_l2") && fact.completed_tokens > 0 && config_.page_size > 0) {
                    fact.prefetch_ready_page_count = fact.completed_tokens / config_.page_size;
                }
                // 旧版 page_hashes_concat 会把 prefix_keys + new_input_tokens 的
                // full path pages 全部写入 target_page_identity。prefetch_schedule
                // 真正需要的是 new_input_tokens 在 target page size 下产生的
                // suffix pages；按目标 page size 从尾部裁剪可兼容旧 trace。
                if (fact.role == "prefetch_schedule" && fact.new_input_tokens > 0 && config_.page_size > 0) {
                    const size_t suffix_pages = static_cast<size_t>(fact.new_input_tokens / config_.page_size);
                    if (suffix_pages > 0 && fact.pages.size() > suffix_pages)
                        fact.pages.erase(fact.pages.begin(), fact.pages.end() - static_cast<long>(suffix_pages));
                }
            }
            else if (fact.role == "prefetch_progress") {
                // `operation_hash_pages` 和 completed token 计数属于 base
                // prefetch operation，不随 target page size 保持页身份不变量。
                // 这里保留 check_return/ongoing/request_id，让 terminal progress
                // 仍能终止 pending prefetch，但不能凭 base 页创建 target ready/L2。
                fact.pages.clear();
                fact.prefetch_ready_page_count = 0;
            }
            else if (fact.requires_page_identity) {
                summary.missing_invariant_facts["target_page_identity_or_token_path"]++;
                continue;
            }
        }
        else if (should_skip_non_invariant_movement(config_, fact)) {
            summary.skipped_non_invariant_events++;
            continue;
        }
        if (fact.pages.empty() && fact.role != "insert" && fact.role != "evict_summary" && fact.role != "prefetch_progress") {
            if (fact.requires_page_identity) summary.missing_page_identity_events++;
            continue;
        }
        auto transitions = state_.apply_fact(fact, summary);
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
    summary.prefetch_planned_pages = sorted_vector(state_.prefetch_planned());
    summary.prefetch_ready_pages = sorted_vector(state_.prefetch_ready());
    summary.prefetch_late_pages = sorted_vector(state_.prefetch_late());
    summary.prefetch_suppressed_pages = sorted_vector(state_.prefetch_suppressed());
    summary.page_hit_counts = state_.hit_counts();

    if (summary.missing_page_identity_events > 0) summary.warnings.push_back("Some HiCache events cannot update state because page_identity is missing.");
    if (!summary.missing_invariant_facts.empty()) summary.warnings.push_back("Some HiCache target-state inputs are missing invariant facts.");
    if (summary.dirty_eviction_events > 0) summary.warnings.push_back("Dirty page eviction triggered modeled writeback state transitions.");
    summary.warnings.push_back("HiCacheModule maintains state only; no DAG mutations are applied.");
    return summary;
}

HiCacheSummary apply_hicache_model(DagGraph & graph, const HiCacheConfig & config) {
    HiCacheStateModel model(config);
    return model.run(graph);
}

} // namespace TraceGraph
