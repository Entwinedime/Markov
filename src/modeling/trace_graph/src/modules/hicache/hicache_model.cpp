#include "trace_graph/modules/hicache/hicache_model.hpp"

#include "trace_graph/core/dag_graph.hpp"
#include "trace_graph/modules/hicache/hicache_router.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>
#include <utility>

namespace TraceGraph {

namespace {

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::vector<std::string> sorted_vector(const std::set<std::string> & values) { return {values.begin(), values.end()}; }

std::string join_scoped_page_sets(const std::unordered_map<std::string, std::set<std::string>> & pages_by_scope) {
    std::map<std::string, std::set<std::string>> ordered(pages_by_scope.begin(), pages_by_scope.end());
    std::ostringstream os;
    bool first_scope = true;
    for (const auto & [scope, pages] : ordered) {
        if (!first_scope) os << "|";
        first_scope = false;
        os << scope << "=";
        bool first_page = true;
        for (const auto & page : pages) {
            if (!first_page) os << ",";
            first_page = false;
            os << page;
        }
    }
    return os.str();
}

std::vector<std::string> slice_pages(const std::vector<std::string> & pages, size_t begin, size_t end) {
    if (begin >= pages.size() || begin >= end) return {};
    end = std::min(end, pages.size());
    return {pages.begin() + static_cast<long>(begin), pages.begin() + static_cast<long>(end)};
}

std::vector<std::string> suffix_pages(const std::vector<std::string> & pages, size_t begin) { return slice_pages(pages, begin, pages.size()); }

HiCacheTokenPath slice_token_path(const HiCacheTokenPath & tokens, size_t begin, size_t end) {
    if (begin >= tokens.size() || begin >= end) return {};
    end = std::min(end, tokens.size());
    return {tokens.begin() + static_cast<long>(begin), tokens.begin() + static_cast<long>(end)};
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

std::string HiCacheState::digest() const {
    return state_index_.digest() + ";pending_writeback=" + join_scoped_page_sets(pending_writeback_pages_by_scope_);
}

HiCacheState::HiCacheState(HiCacheConfig config) : config_(std::move(config)), target_pager_(config_) {}

std::set<std::string> HiCacheState::pending_writeback() const {
    std::set<std::string> pages;
    for (const auto & [scope, pending_pages] : pending_writeback_pages_by_scope_) {
        (void)scope;
        pages.insert(pending_pages.begin(), pending_pages.end());
    }
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
    return static_cast<uint64_t>(std::count_if(pages->begin(), pages->end(), [&](const auto & page) { return page_in_scope(page, scope); }));
}

HiCacheTokenRadixTree & HiCacheState::radix_tree_for_fact(const HiCacheFact & fact) { return radix_trees_[normalized_scope(fact)]; }

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
    case HiCacheFactRole::DiagnosticStateInjection:
        apply_diagnostic_state_injection(fact, summary, transitions);
        break;
    case HiCacheFactRole::Unknown:
        break;
    }
    return transitions;
}

std::vector<HiCacheStateTransition> HiCacheState::finalize(HiCacheSummary & summary) {
    std::vector<HiCacheStateTransition> transitions;
    if (config_.prefetch_policy == "best_effort") {
        HiCacheFact fact;
        fact.role = "prefetch_finalize";
        fact.event_name = "hicache_prefetch_finalize";
        for (const auto & page : state_index_.prefetch_planned()) {
            if (state_index_.prefetch_ready().count(page) == 0) mark_prefetch_suppressed(fact, summary, transitions, page);
        }
    }
    return transitions;
}

void HiCacheState::apply_request_tokens(const HiCacheFact & fact) {
    token_store_.set_request_tokens(fact, fact.full_path_tokens);
    token_store_.observe_request_bound_tokens(fact, fact.full_path_tokens);
}

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

std::vector<std::string> HiCacheState::flatten_page_groups(const std::vector<std::vector<std::string>> & groups) const {
    std::vector<std::string> pages;
    std::set<std::string> seen;
    for (const auto & group : groups) {
        for (const auto & page : group) {
            if (!page.empty() && seen.insert(page).second) pages.push_back(page);
        }
    }
    return pages;
}

void HiCacheState::update_request_path_state(const HiCacheFact & fact, const std::vector<std::string> & full_pages,
                                             const std::vector<std::string> & matched_pages,
                                             const std::vector<std::vector<std::string>> & chain_groups) {
    const auto key = scoped_request_key(fact);
    if (key.empty()) return;
    auto & state = request_states_[key];
    state.full_pages = full_pages;
    state.matched_device_prefix_pages = matched_pages;
    state.last_device_chain_groups = chain_groups;
}

uint64_t HiCacheState::active_device_reservation_pages_for_scope(const std::string & scope) const {
    uint64_t pages = 0;
    const auto prefix = scope + ":";
    for (const auto & [request_key, state] : request_states_) {
        if (request_key.rfind(prefix, 0) == 0) pages += state.device_reservation_pages;
    }
    return pages;
}

uint64_t HiCacheState::estimate_admission_requested_tokens(const HiCacheFact & fact, const RequestExecutionState & request_state) const {
    const auto page_size = page_size_for_fact(fact);
    if (page_size == 0) return 0;
    const auto full_tokens = full_path_token_count(fact);
    const auto matched_tokens = static_cast<uint64_t>(request_state.matched_device_prefix_pages.size()) * page_size;
    const auto extend_tokens = full_tokens > matched_tokens ? full_tokens - matched_tokens : 0;
    return extend_tokens + page_size;
}

uint64_t HiCacheState::estimate_admission_requested_pages(const HiCacheFact & fact, const RequestExecutionState & request_state) const {
    const auto page_size = page_size_for_fact(fact);
    if (page_size == 0) return 0;
    return ceil_div(estimate_admission_requested_tokens(fact, request_state), page_size);
}

void HiCacheState::clear_device_reservation(const std::string & request_key) {
    auto it = request_states_.find(request_key);
    if (it != request_states_.end()) it->second.device_reservation_pages = 0;
}

void HiCacheState::acquire_device_request_lock(const HiCacheFact & fact, HiCacheSummary & summary,
                                               std::vector<HiCacheStateTransition> & transitions, const std::string & request_key,
                                               const std::vector<std::string> & pages) {
    if (request_key.empty()) return;
    std::set<std::string> desired;
    for (const auto & page : pages) {
        if (!page.empty()) desired.insert(page);
    }
    if (desired.empty()) return;
    auto & held = request_device_lock_pages_[request_key];
    for (const auto & page : desired) {
        if (!held.insert(page).second) continue;
        const auto before = device_lock_count_by_page_[page]++;
        if (before == 0) mark_locked(fact, summary, transitions, page);
    }
}

void HiCacheState::release_device_request_lock(const HiCacheFact & fact, HiCacheSummary & summary,
                                               std::vector<HiCacheStateTransition> & transitions, const std::string & request_key) {
    auto it = request_device_lock_pages_.find(request_key);
    if (it == request_device_lock_pages_.end()) return;
    for (const auto & page : it->second) {
        auto count_it = device_lock_count_by_page_.find(page);
        if (count_it == device_lock_count_by_page_.end() || count_it->second == 0) continue;
        count_it->second--;
        if (count_it->second == 0) {
            device_lock_count_by_page_.erase(count_it);
            clear_locked(fact, summary, transitions, page);
        }
    }
    request_device_lock_pages_.erase(it);
}

void HiCacheState::replace_device_request_lock(const HiCacheFact & fact, HiCacheSummary & summary,
                                               std::vector<HiCacheStateTransition> & transitions, const std::string & request_key,
                                               const std::vector<std::string> & pages) {
    release_device_request_lock(fact, summary, transitions, request_key);
    acquire_device_request_lock(fact, summary, transitions, request_key, pages);
}

void HiCacheState::apply_target_device_pressure(const HiCacheFact & fact, HiCacheSummary & summary,
                                                std::vector<HiCacheStateTransition> & transitions, uint64_t requested_pages) {
    if (requested_pages == 0 || config_.l1_capacity_pages == 0) return;
    const auto scope = normalized_scope(fact);
    const auto occupied = tier_size_for_scope("L1", scope) + active_device_reservation_pages_for_scope(scope);
    if (occupied + requested_pages <= config_.l1_capacity_pages) return;
    evict_lru_pages(fact, summary, transitions, "L1", requested_pages);
}

void HiCacheState::apply_request_admission(const HiCacheFact & fact, HiCacheSummary & summary,
                                           std::vector<HiCacheStateTransition> & transitions) {
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

    auto & radix_tree = radix_tree_for_fact(fact);
    auto match = radix_tree.match_page_path(full_pages);
    update_request_path_state(fact, full_pages, match.matched_pages, match.ancestor_page_groups);

    const auto request_key = scoped_request_key(fact);
    if (request_key.empty()) return;
    auto & request_state = request_states_[request_key];
    const auto lock_pages = flatten_page_groups(request_state.last_device_chain_groups);
    acquire_device_request_lock(fact, summary, transitions, request_key, lock_pages);

    const auto requested_pages = estimate_admission_requested_pages(fact, request_state);
    apply_target_device_pressure(fact, summary, transitions, requested_pages);
    request_state.device_reservation_pages = requested_pages;
    request_state.lifecycle_state = "admitted";
}

void HiCacheState::apply_request_lifecycle_anchor(const HiCacheFact & fact, HiCacheSummary & summary,
                                                  std::vector<HiCacheStateTransition> & transitions) {
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
    if (kind == "unfinished") {
        replace_device_request_lock(fact, summary, transitions, request_key, lock_pages);
        state_it->second.lifecycle_state = "unfinished";
    }
    else {
        release_device_request_lock(fact, summary, transitions, request_key);
        request_states_.erase(state_it);
    }
}

void HiCacheState::apply_request_bound_match_anchor(const HiCacheFact & fact, HiCacheSummary & summary,
                                                    std::vector<HiCacheStateTransition> & transitions) {
    auto tokens = projected_full_path_tokens_for_fact(fact);
    if (tokens.empty()) {
        note_missing_projection_if_needed(fact, summary);
        return;
    }
    auto pages = pages_for_tokens(fact, tokens);
    if (pages.empty()) return;
    token_store_.observe_request_bound_tokens(fact, tokens);

    auto & radix_tree = radix_tree_for_fact(fact);
    auto match = radix_tree.match_page_path(pages);
    auto matched_pages = match.matched_pages;
    update_request_path_state(fact, pages, matched_pages, match.ancestor_page_groups);
    for (const auto & page : matched_pages) {
        if (state_index_.l1().count(page) > 0) {
            touch_page("L1", page);
            if (state_index_.l2().count(page) > 0) touch_page("L2", page);
            continue;
        }
        if (state_index_.l2().count(page) > 0) {
            touch_page("L2", page);
            add_resident(fact, summary, transitions, "L1", page);
            continue;
        }
        if (state_index_.l3().count(page) > 0) {
            add_resident(fact, summary, transitions, "L2", page);
            add_resident(fact, summary, transitions, "L1", page);
            if (config_.prefetch_policy == "wait_complete" && state_index_.prefetch_planned().count(page) > 0)
                mark_prefetch_ready(fact, summary, transitions, page);
        }
    }
    enforce_capacity(fact, summary, transitions, "L2");
    enforce_capacity(fact, summary, transitions, "L1");
}

void HiCacheState::apply_request_lifecycle_insert(const HiCacheFact & fact, HiCacheSummary & summary,
                                                  std::vector<HiCacheStateTransition> & transitions) {
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

    auto & radix_tree = radix_tree_for_fact(fact);
    const auto prefix_pages = radix_tree.longest_prefix_pages(tokens, page_size_for_fact(fact));
    auto inserted_pages = suffix_pages(full_pages, prefix_pages);
    auto inserted_match = radix_tree.insert_path(tokens, full_pages);
    update_request_path_state(fact, full_pages, full_pages, inserted_match.ancestor_page_groups);
    if (inserted_pages.empty()) {
        apply_write_policy_hit_counts(fact, full_pages, summary, transitions);
        return;
    }

    for (const auto & page : inserted_pages) {
        add_resident(fact, summary, transitions, "L1", page);
        if (config_.write_policy == "write_through") {
            add_resident(fact, summary, transitions, "L2", page);
            add_resident(fact, summary, transitions, "L3", page);
            mark_backuped(fact, summary, transitions, page);
            clear_dirty(fact, summary, transitions, page);
        }
        else {
            mark_dirty(fact, summary, transitions, page);
        }
    }
    apply_write_policy_hit_counts(fact, full_pages, summary, transitions);
    enforce_capacity(fact, summary, transitions, "L1");
    enforce_capacity(fact, summary, transitions, "L2");
}

void HiCacheState::apply_prefetch_decision(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    auto tokens = projected_full_path_tokens_for_fact(fact);
    if (tokens.empty()) {
        note_missing_projection_if_needed(fact, summary);
        return;
    }
    auto full_pages = pages_for_tokens(fact, tokens);
    if (full_pages.empty()) return;
    token_store_.set_request_tokens(fact, tokens);

    auto & radix_tree = radix_tree_for_fact(fact);
    const auto prefix_pages = radix_tree.longest_prefix_pages(tokens, page_size_for_fact(fact));
    auto pages = suffix_pages(full_pages, prefix_pages);
    if (pages.empty()) return;
    if (!fact.request_id.empty()) {
        const auto key = scoped_request_key(fact);
        pending_prefetch_pages_[key] = pages;
        prefetch_decision_ts_[key] = fact.ts;
        active_prefetch_host_pages_by_request_[key] = static_cast<uint64_t>(pages.size());
        pending_prefetch_host_pressure_requests_.insert(key);
    }
    for (const auto & page : pages) mark_prefetch_planned(fact, summary, transitions, page);
}

void HiCacheState::apply_prefetch_check_point(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    if (fact.request_id.empty()) return;
    const auto key = scoped_request_key(fact);
    auto it = pending_prefetch_pages_.find(key);
    if (it == pending_prefetch_pages_.end() || it->second.empty()) return;
    const auto & pages = it->second;
    apply_pending_prefetch_host_pressure_for_request(fact, summary, transitions, key);
    if (config_.prefetch_policy == "wait_complete") {
        release_prefetch_host_buffer(fact);
        pending_prefetch_pages_.erase(it);
        return;
    }
    if (config_.prefetch_policy == "best_effort") {
        for (const auto & page : pages) {
            if (state_index_.prefetch_ready().count(page) == 0) mark_prefetch_suppressed(fact, summary, transitions, page);
        }
        release_prefetch_host_buffer(fact);
        pending_prefetch_pages_.erase(it);
        return;
    }
    if (config_.prefetch_policy != "timeout" || !config_.prefetch_timeout_configured) return;
    auto ts_it = prefetch_decision_ts_.find(key);
    if (ts_it == prefetch_decision_ts_.end() || fact.ts < ts_it->second) return;
    const auto page_size = page_size_for_fact(fact);
    const auto token_count = static_cast<uint64_t>(pages.size()) * page_size;
    double timeout_sec = config_.prefetch_timeout_base_sec + config_.prefetch_timeout_per_ki_token_sec * static_cast<double>(token_count) / 1024.0;
    if (config_.prefetch_timeout_max_sec > 0.0) timeout_sec = std::min(timeout_sec, config_.prefetch_timeout_max_sec);
    const double elapsed_sec = static_cast<double>(fact.ts - ts_it->second) / 1000000.0;
    if (elapsed_sec < timeout_sec) return;
    for (const auto & page : pages) {
        if (state_index_.prefetch_ready().count(page) == 0) mark_prefetch_late(fact, summary, transitions, page);
    }
    release_prefetch_host_buffer(fact);
    pending_prefetch_pages_.erase(it);
}

std::string HiCacheState::page_for_scope(const HiCacheFact & fact, const std::string & page) const {
    if (page.find('|') != std::string::npos) return page;
    return normalized_scope(fact) + "|" + page;
}

std::set<std::string> HiCacheState::diagnostic_desired_pages(const HiCacheFact & fact, const std::string & state_key) const {
    std::set<std::string> pages;
    auto it = fact.diagnostic_state_pages.find(state_key);
    if (it == fact.diagnostic_state_pages.end()) return pages;
    for (const auto & page : it->second) {
        if (!page.empty()) pages.insert(page_for_scope(fact, page));
    }
    return pages;
}

void HiCacheState::replace_resident_state_for_scope(const HiCacheFact & fact, HiCacheSummary & summary,
                                                    std::vector<HiCacheStateTransition> & transitions, const std::string & state_key,
                                                    const std::string & tier) {
    if (fact.diagnostic_state_pages.count(state_key) == 0) return;
    auto * pages = tier_set(tier);
    if (!pages) return;
    const auto scope = normalized_scope(fact);
    const auto desired = diagnostic_desired_pages(fact, state_key);
    std::set<std::string> current;
    for (const auto & page : *pages) {
        if (page_in_scope(page, scope)) current.insert(page);
    }
    for (const auto & page : current) {
        if (desired.count(page) == 0) remove_resident(fact, summary, transitions, tier, page);
    }
    for (const auto & page : desired) {
        if (current.count(page) == 0) add_resident(fact, summary, transitions, tier, page);
    }
}

void HiCacheState::replace_metadata_state_for_scope(const HiCacheFact & fact, HiCacheSummary & summary,
                                                    std::vector<HiCacheStateTransition> & transitions, const std::string & state_key) {
    if (fact.diagnostic_state_pages.count(state_key) == 0) return;
    const std::set<std::string> * source = nullptr;
    if (state_key == "dirty_pages")
        source = &state_index_.dirty();
    else if (state_key == "backuped_pages")
        source = &state_index_.backuped();
    else if (state_key == "evicted_pages")
        source = &state_index_.evicted();
    else if (state_key == "locked_pages")
        source = &state_index_.locked();
    else
        return;

    const auto scope = normalized_scope(fact);
    const auto desired = diagnostic_desired_pages(fact, state_key);
    std::set<std::string> current;
    for (const auto & page : *source) {
        if (page_in_scope(page, scope)) current.insert(page);
    }
    for (const auto & page : current) {
        if (desired.count(page) > 0) continue;
        if (state_key == "dirty_pages")
            clear_dirty(fact, summary, transitions, page);
        else if (state_key == "backuped_pages")
            clear_backuped(fact, summary, transitions, page);
        else if (state_key == "evicted_pages")
            clear_evicted(fact, summary, transitions, page);
        else if (state_key == "locked_pages")
            clear_locked(fact, summary, transitions, page);
    }
    for (const auto & page : desired) {
        if (current.count(page) > 0) continue;
        if (state_key == "dirty_pages")
            mark_dirty(fact, summary, transitions, page);
        else if (state_key == "backuped_pages")
            mark_backuped(fact, summary, transitions, page);
        else if (state_key == "evicted_pages")
            mark_evicted(fact, summary, transitions, page);
        else if (state_key == "locked_pages")
            mark_locked(fact, summary, transitions, page);
    }
}

void HiCacheState::apply_diagnostic_state_injection(const HiCacheFact & fact, HiCacheSummary & summary,
                                                    std::vector<HiCacheStateTransition> & transitions) {
    replace_resident_state_for_scope(fact, summary, transitions, "l1_resident_pages", "L1");
    replace_resident_state_for_scope(fact, summary, transitions, "l2_resident_pages", "L2");
    replace_metadata_state_for_scope(fact, summary, transitions, "dirty_pages");
    replace_metadata_state_for_scope(fact, summary, transitions, "backuped_pages");
    replace_metadata_state_for_scope(fact, summary, transitions, "evicted_pages");
    replace_metadata_state_for_scope(fact, summary, transitions, "locked_pages");
}

void HiCacheState::apply_write_policy_hit_counts(const HiCacheFact & fact, const std::vector<std::string> & full_pages, HiCacheSummary & summary,
                                                 std::vector<HiCacheStateTransition> & transitions) {
    if (!target_write_count_enabled()) return;
    const auto threshold = target_write_through_threshold();
    if (threshold == 0) return;
    std::set<std::string> counted;
    for (const auto & page : full_pages) {
        if (page.empty() || !counted.insert(page).second) continue;
        if (state_index_.l1().count(page) == 0 && !radix_tree_for_fact(fact).contains_page(page)) continue;
        auto before = transition_state_digest();
        const auto hit_count = state_index_.increment_hit_count(fact.cache_scope, page);
        record_transition(fact, summary, transitions, "increment_hit_count", "", page, before);
        if (hit_count < threshold) continue;
        add_resident(fact, summary, transitions, "L2", page);
        add_resident(fact, summary, transitions, "L3", page);
        mark_backuped(fact, summary, transitions, page);
        clear_dirty(fact, summary, transitions, page);
    }
}

uint64_t HiCacheState::active_prefetch_host_pages_for_scope(const std::string & scope) const {
    uint64_t pages = 0;
    const auto prefix = scope + ":";
    for (const auto & [request_key, count] : active_prefetch_host_pages_by_request_) {
        if (request_key.rfind(prefix, 0) == 0) pages += count;
    }
    return pages;
}

void HiCacheState::apply_pending_prefetch_host_pressure_for_request(const HiCacheFact & fact, HiCacheSummary & summary,
                                                                    std::vector<HiCacheStateTransition> & transitions,
                                                                    const std::string & request_key) {
    auto pending_it = pending_prefetch_host_pressure_requests_.find(request_key);
    if (pending_it == pending_prefetch_host_pressure_requests_.end()) return;
    auto active_it = active_prefetch_host_pages_by_request_.find(request_key);
    if (active_it != active_prefetch_host_pages_by_request_.end() && active_it->second > 0) {
        apply_prefetch_host_pressure(fact, summary, transitions, active_it->second);
    }
    pending_prefetch_host_pressure_requests_.erase(pending_it);
}

void HiCacheState::release_prefetch_host_buffer(const HiCacheFact & fact) {
    const auto key = scoped_request_key(fact);
    if (!key.empty()) {
        active_prefetch_host_pages_by_request_.erase(key);
        pending_prefetch_host_pressure_requests_.erase(key);
        prefetch_decision_ts_.erase(key);
    }
}

void HiCacheState::add_resident(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, const std::string & tier,
                                const std::string & page) {
    auto before = transition_state_digest();
    if (state_index_.add_resident(tier, page)) record_transition(fact, summary, transitions, "add_" + lower(tier) + "_resident", tier, page, before);
    if (tier == "L2") mark_backuped(fact, summary, transitions, page);
}

void HiCacheState::remove_resident(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                   const std::string & tier, const std::string & page) {
    auto before = transition_state_digest();
    if (state_index_.remove_resident(tier, page)) {
        record_transition(fact, summary, transitions, "remove_" + lower(tier) + "_resident", tier, page, before);
        if (tier == "L2") clear_backuped(fact, summary, transitions, page);
    }
}

void HiCacheState::mark_dirty(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions, const std::string & page) {
    auto before = transition_state_digest();
    if (state_index_.mark_dirty(page)) record_transition(fact, summary, transitions, "mark_dirty", "", page, before);
}

void HiCacheState::clear_dirty(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                               const std::string & page) {
    auto before = transition_state_digest();
    if (state_index_.clear_dirty(page)) record_transition(fact, summary, transitions, "clear_dirty", "", page, before);
}

void HiCacheState::mark_backuped(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                 const std::string & page) {
    auto before = transition_state_digest();
    if (state_index_.mark_backuped(page)) record_transition(fact, summary, transitions, "mark_backuped", "", page, before);
    clear_dirty(fact, summary, transitions, page);
}

void HiCacheState::clear_backuped(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                  const std::string & page) {
    auto before = transition_state_digest();
    if (state_index_.clear_backuped(page)) record_transition(fact, summary, transitions, "clear_backuped", "", page, before);
}

void HiCacheState::mark_evicted(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                const std::string & page) {
    auto before = transition_state_digest();
    if (state_index_.mark_evicted(page)) record_transition(fact, summary, transitions, "mark_evicted", "", page, before);
}

void HiCacheState::clear_evicted(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                 const std::string & page) {
    auto before = transition_state_digest();
    if (state_index_.clear_evicted(page)) record_transition(fact, summary, transitions, "clear_evicted", "", page, before);
}

void HiCacheState::mark_locked(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                               const std::string & page) {
    auto before = transition_state_digest();
    if (state_index_.mark_locked(page)) record_transition(fact, summary, transitions, "mark_locked", "", page, before);
}

void HiCacheState::clear_locked(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                const std::string & page) {
    auto before = transition_state_digest();
    if (state_index_.clear_locked(page)) record_transition(fact, summary, transitions, "clear_locked", "", page, before);
}

void HiCacheState::mark_prefetch_planned(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                         const std::string & page) {
    auto before = transition_state_digest();
    if (state_index_.mark_prefetch_planned(page)) record_transition(fact, summary, transitions, "mark_prefetch_planned", "", page, before);
}

void HiCacheState::mark_prefetch_ready(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                       const std::string & page) {
    auto before = transition_state_digest();
    if (state_index_.mark_prefetch_ready(page)) record_transition(fact, summary, transitions, "mark_prefetch_ready", "", page, before);
}

void HiCacheState::mark_prefetch_late(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                      const std::string & page) {
    auto before = transition_state_digest();
    if (state_index_.mark_prefetch_late(page)) record_transition(fact, summary, transitions, "mark_prefetch_late", "", page, before);
}

void HiCacheState::mark_prefetch_suppressed(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                            const std::string & page) {
    auto before = transition_state_digest();
    if (state_index_.mark_prefetch_suppressed(page)) record_transition(fact, summary, transitions, "mark_prefetch_suppressed", "", page, before);
}

void HiCacheState::touch_page(const std::string & tier, const std::string & page) { state_index_.touch_page(tier, page); }

void HiCacheState::flush_dirty_page_to_host(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                            const std::string & page) {
    add_resident(fact, summary, transitions, "L2", page);
    mark_backuped(fact, summary, transitions, page);
    clear_dirty(fact, summary, transitions, page);
}

void HiCacheState::enqueue_writeback_page(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                          const std::string & page) {
    const auto scope = normalized_scope(fact);
    auto before = transition_state_digest();
    if (pending_writeback_pages_by_scope_[scope].insert(page).second) record_transition(fact, summary, transitions, "enqueue_writeback", "", page, before);
}

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
    add_resident(fact, summary, transitions, "L2", page);
    mark_backuped(fact, summary, transitions, page);
    clear_dirty(fact, summary, transitions, page);
}

void HiCacheState::apply_prefetch_host_pressure(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                                uint64_t requested_pages) {
    if (requested_pages == 0 || config_.l2_capacity_pages == 0) return;
    const auto scope = normalized_scope(fact);
    const auto occupied_pages = tier_size_for_scope("L2", scope) + active_prefetch_host_pages_for_scope(scope);
    if (occupied_pages <= config_.l2_capacity_pages) return;
    evict_host_pages(fact, summary, transitions, requested_pages);
}

void HiCacheState::evict_host_pages(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                    uint64_t page_count) {
    auto * pages = tier_set("L2");
    auto * order = touch_order_for_tier("L2");
    if (!pages || !order || page_count == 0) return;
    const auto scope = normalized_scope(fact);
    auto & radix_tree = radix_tree_for_fact(fact);
    uint64_t removed = 0;
    std::set<std::string> skipped_locked;
    while (removed < page_count) {
        std::set<std::string> host_pages;
        std::set<std::string> evicted_pages;
        std::set<std::string> locked_pages;
        for (const auto & page : *pages) {
            if (!page_in_scope(page, scope)) continue;
            host_pages.insert(page);
            if (state_index_.evicted().count(page) > 0) evicted_pages.insert(page);
            if (state_index_.locked().count(page) > 0) locked_pages.insert(page);
        }
        std::set<std::string> eligible_pages;
        for (const auto & page : evicted_pages) {
            if (locked_pages.count(page) == 0) eligible_pages.insert(page);
        }
        if (eligible_pages.empty()) break;

        auto candidate_groups = radix_tree.host_eviction_leaf_groups(host_pages, evicted_pages, locked_pages);
        std::vector<std::string> victims;
        if (!candidate_groups.empty()) {
            size_t best_group = candidate_groups.size();
            size_t best_order_index = order->size();
            for (size_t group_index = 0; group_index < candidate_groups.size(); ++group_index) {
                size_t group_order_index = order->size();
                for (const auto & page : candidate_groups[group_index]) {
                    const auto order_it = std::find(order->begin(), order->end(), page);
                    if (order_it == order->end()) continue;
                    group_order_index = std::min(group_order_index, static_cast<size_t>(std::distance(order->begin(), order_it)));
                }
                if (group_order_index < best_order_index) {
                    best_order_index = group_order_index;
                    best_group = group_index;
                }
            }
            if (best_group < candidate_groups.size()) victims = candidate_groups[best_group];
        }
        if (victims.empty()) {
            auto victim_it = std::find_if(order->begin(), order->end(), [&](const auto & page) { return eligible_pages.count(page) > 0; });
            if (victim_it == order->end()) break;
            const auto victim = *victim_it;
            order->erase(victim_it);
            victims = radix_tree.leaf_group_for_page(victim);
            if (victims.empty()) victims = {victim};
            bool protected_group = false;
            for (const auto & page : victims) {
                if (state_index_.locked().count(page) > 0) {
                    protected_group = true;
                    break;
                }
            }
            if (protected_group) {
                skipped_locked.insert(victim);
                continue;
            }
        }
        for (const auto & page : victims) {
            if (!pages->count(page) || state_index_.evicted().count(page) == 0) continue;
            order->erase(std::remove(order->begin(), order->end(), page), order->end());
            remove_resident(fact, summary, transitions, "L2", page);
            clear_evicted(fact, summary, transitions, page);
            removed++;
        }
    }
    for (const auto & page : skipped_locked) {
        if (pages->count(page) > 0) order->push_back(page);
    }
}

void HiCacheState::evict_lru_pages(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                   const std::string & tier, uint64_t page_count) {
    auto * pages = tier_set(tier);
    auto * order = touch_order_for_tier(tier);
    if (!pages || !order || page_count == 0) return;
    const auto scope = normalized_scope(fact);
    auto & radix_tree = radix_tree_for_fact(fact);
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
                if (state_index_.locked().count(page) > 0) locked_pages.insert(page);
            }
            if (dynamic_device_tree_ready) {
                auto candidate_groups = radix_tree.device_eviction_leaf_groups(device_pages, locked_pages);
                size_t best_group = candidate_groups.size();
                size_t best_order_index = order->size();
                for (size_t group_index = 0; group_index < candidate_groups.size(); ++group_index) {
                    size_t group_order_index = order->size();
                    for (const auto & page : candidate_groups[group_index]) {
                        const auto order_it = std::find(order->begin(), order->end(), page);
                        if (order_it == order->end()) continue;
                        group_order_index = std::min(group_order_index, static_cast<size_t>(std::distance(order->begin(), order_it)));
                    }
                    if (group_order_index < best_order_index) {
                        best_order_index = group_order_index;
                        best_group = group_index;
                    }
                }
                if (best_group == candidate_groups.size()) break;
                victims = candidate_groups[best_group];
            }
        }
        if (victims.empty()) {
            auto victim_it =
                std::find_if(order->begin(), order->end(), [&](const auto & page) { return pages->count(page) > 0 && page_in_scope(page, scope); });
            if (victim_it == order->end()) break;
            auto victim = *victim_it;
            order->erase(victim_it);
            if (!pages->count(victim)) continue;
            victims = radix_tree.leaf_group_for_page(victim);
            if (victims.empty()) victims = {victim};
        }
        bool locked_group = false;
        for (const auto & page : victims) {
            if (state_index_.locked().count(page) > 0) {
                locked_group = true;
                break;
            }
        }
        if (locked_group) {
            skipped_locked.insert(victims.front());
            continue;
        }
        for (const auto & page : victims) {
            if (!pages->count(page)) continue;
            order->erase(std::remove(order->begin(), order->end(), page), order->end());
            if (state_index_.dirty().count(page)) {
                summary.dirty_eviction_events++;
                if (config_.write_policy == "write_back") {
                    // HiRadixCache capacity eviction blocks for write-back ACKs
                    // before removing the device block.
                    enqueue_writeback_page(fact, summary, transitions, page);
                    complete_writeback_page(fact, summary, transitions, page);
                }
                else
                    flush_dirty_page_to_host(fact, summary, transitions, page);
            }
            remove_resident(fact, summary, transitions, tier, page);
            if (tier == "L1")
                mark_evicted(fact, summary, transitions, page);
            else
                clear_evicted(fact, summary, transitions, page);
            removed++;
        }
    }
    for (const auto & page : skipped_locked) {
        if (pages->count(page) > 0) order->push_back(page);
    }
}

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

    std::stable_sort(facts.begin(), facts.end(), [](const HiCacheFact & left, const HiCacheFact & right) {
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
    if (summary.processed_events_by_role.count("diagnostic_state_injection") > 0)
        summary.warnings.push_back("Diagnostic oracle state injections were applied; this run is boundary-elided diagnosis, not a normal prediction.");
    summary.warnings.push_back(
        "HiCacheModule consumes only invariant_state facts; source_actual/timing_observation/oracle_state are ignored for target state.");
    summary.warnings.push_back("HiCacheModule maintains state only; no DAG mutations are applied.");
    return summary;
}

HiCacheSummary apply_hicache_model(DagGraph & graph, const HiCacheConfig & config) {
    HiCacheStateModel model(config);
    return model.run(graph);
}

} // namespace TraceGraph
