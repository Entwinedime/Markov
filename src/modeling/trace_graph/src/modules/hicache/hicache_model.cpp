#include "trace_graph/modules/hicache/hicache_model.hpp"

#include "trace_graph/core/dag_graph.hpp"

#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace TraceGraph {

namespace {

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::vector<std::string> sorted_vector(const std::set<std::string> & values) { return {values.begin(), values.end()}; }

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

std::vector<std::string> slice_pages(const std::vector<std::string> & pages, size_t begin, size_t end) {
    if (begin >= pages.size() || begin >= end) return {};
    end = std::min(end, pages.size());
    return {pages.begin() + static_cast<long>(begin), pages.begin() + static_cast<long>(end)};
}

std::vector<std::string> suffix_pages(const std::vector<std::string> & pages, size_t begin) { return slice_pages(pages, begin, pages.size()); }

std::vector<unsigned char> hex_to_bytes(const std::string & hex) {
    std::vector<unsigned char> bytes;
    if (hex.size() % 2 != 0) return bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        try {
            bytes.push_back(static_cast<unsigned char>(std::stoul(hex.substr(i, 2), nullptr, 16)));
        }
        catch (...) {
            return {};
        }
    }
    return bytes;
}

std::string bytes_to_hex(const unsigned char * bytes, size_t len) {
    std::ostringstream os;
    os << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; ++i) os << std::setw(2) << static_cast<unsigned int>(bytes[i]);
    return os.str();
}

std::string hash_token_page(const HiCacheTokenPath & tokens, size_t begin, size_t end, const std::string & prior_hash) {
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    if (!prior_hash.empty()) {
        auto parent = hex_to_bytes(prior_hash);
        if (!parent.empty()) SHA256_Update(&ctx, parent.data(), parent.size());
    }
    for (size_t index = begin; index < end && index < tokens.size(); ++index) {
        for (const auto word : tokens[index].words) {
            unsigned char raw[4] = {
                static_cast<unsigned char>(word & 0xffu),
                static_cast<unsigned char>((word >> 8u) & 0xffu),
                static_cast<unsigned char>((word >> 16u) & 0xffu),
                static_cast<unsigned char>((word >> 24u) & 0xffu),
            };
            SHA256_Update(&ctx, raw, sizeof(raw));
        }
    }
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256_Final(digest.data(), &ctx);
    return bytes_to_hex(digest.data(), digest.size());
}

uint64_t ceil_div(uint64_t value, uint64_t divisor) {
    if (value == 0 || divisor == 0) return 0;
    return (value + divisor - 1) / divisor;
}

bool is_invariant_state_fact(const HiCacheFact & fact) { return fact.fact_class == "invariant_state" && fact.state_model_input; }

bool is_known_invariant_role(const std::string & role) {
    static const std::unordered_set<std::string> roles = {
        "request_tokens",
        "lookup_path",
        "cache_config_observed",
        "insert_path",
        "prefetch_intent",
        "prefetch_check_point",
        "capacity_request",
        "lock_scope_delta",
    };
    return roles.count(role) > 0;
}

} // namespace

std::string HiCacheState::digest() const {
    std::ostringstream os;
    os << "l1=" << join_set(l1_) << ";l2=" << join_set(l2_) << ";l3=" << join_set(l3_) << ";dirty=" << join_set(dirty_) << ";backuped="
       << join_set(backuped_) << ";evicted=" << join_set(evicted_) << ";prefetch_planned=" << join_set(prefetch_planned_)
       << ";prefetch_ready=" << join_set(prefetch_ready_) << ";prefetch_late=" << join_set(prefetch_late_) << ";prefetch_suppressed="
       << join_set(prefetch_suppressed_) << ";locked=" << join_set(locked_) << ";hit_count=" << join_count_map(hit_count_by_scope_page_);
    return os.str();
}

HiCacheState::HiCacheState(HiCacheConfig config) : config_(std::move(config)) {}

uint64_t HiCacheState::page_size_for_fact(const HiCacheFact & fact) const {
    if (config_.page_size > 0) return config_.page_size;
    return fact.source_page_size;
}

std::string HiCacheState::scoped_page_id(const HiCacheFact & fact, const std::string & page_hash) const {
    const auto scope = fact.cache_scope.empty() ? std::string("-1") : fact.cache_scope;
    return scope + "|" + page_hash;
}

std::vector<std::string> HiCacheState::pages_for_tokens(const HiCacheFact & fact, const HiCacheTokenPath & tokens) const {
    const auto page_size = page_size_for_fact(fact);
    if (page_size == 0 || tokens.size() < page_size) return {};
    const auto aligned_len = tokens.size() / page_size * page_size;
    std::vector<std::string> pages;
    pages.reserve(aligned_len / page_size);
    std::string parent_hash;
    for (size_t begin = 0; begin < aligned_len; begin += static_cast<size_t>(page_size)) {
        const auto end = begin + static_cast<size_t>(page_size);
        parent_hash = hash_token_page(tokens, begin, end, parent_hash);
        pages.push_back(scoped_page_id(fact, parent_hash));
    }
    return pages;
}

std::vector<std::string> HiCacheState::suffix_pages_for_prefetch(const HiCacheFact & fact) const {
    const auto page_size = page_size_for_fact(fact);
    if (page_size == 0) return {};
    if (!fact.full_path_tokens.empty()) {
        auto full_pages = pages_for_tokens(fact, fact.full_path_tokens);
        const auto prefix_pages = fact.prefix_tokens.size() / page_size;
        if (prefix_pages >= full_pages.size()) return {};
        return suffix_pages(full_pages, static_cast<size_t>(prefix_pages));
    }
    return pages_for_tokens(fact, fact.suffix_tokens);
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

std::vector<HiCacheStateTransition> HiCacheState::apply_fact(const HiCacheFact & fact, HiCacheSummary & summary) {
    std::vector<HiCacheStateTransition> transitions;
    if (fact.role == "request_tokens") {
        apply_request_tokens(fact);
        return transitions;
    }
    if (fact.role == "lookup_path") {
        apply_lookup_path(fact, summary, transitions);
        return transitions;
    }
    if (fact.role == "insert_path") {
        apply_insert_path(fact, summary, transitions);
        return transitions;
    }
    if (fact.role == "prefetch_intent") {
        apply_prefetch_intent(fact, summary, transitions);
        return transitions;
    }
    if (fact.role == "prefetch_check_point") {
        apply_prefetch_check_point(fact, summary, transitions);
        return transitions;
    }
    if (fact.role == "capacity_request") {
        apply_capacity_request(fact, summary, transitions);
        return transitions;
    }
    if (fact.role == "lock_scope_delta") {
        apply_lock_scope_delta(fact, summary, transitions);
        return transitions;
    }
    return transitions;
}

std::vector<HiCacheStateTransition> HiCacheState::finalize(HiCacheSummary & summary) {
    std::vector<HiCacheStateTransition> transitions;
    if (config_.prefetch_policy == "best_effort" || config_.prefetch_policy == "wait_complete") {
        HiCacheFact fact;
        fact.role = "prefetch_finalize";
        fact.event_name = "hicache_prefetch_finalize";
        for (const auto & page : prefetch_planned_) {
            if (prefetch_ready_.count(page) == 0) mark_prefetch_suppressed(fact, summary, transitions, page);
        }
    }
    return transitions;
}

void HiCacheState::apply_request_tokens(const HiCacheFact & fact) {
    const auto key = scoped_request_key(fact);
    if (key.empty()) return;
    auto pages = pages_for_tokens(fact, fact.full_path_tokens);
    if (!pages.empty()) request_pages_[key] = std::move(pages);
}

void HiCacheState::apply_lookup_path(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    auto pages = pages_for_tokens(fact, fact.full_path_tokens);
    if (pages.empty() && !fact.request_id.empty()) {
        auto it = request_pages_.find(scoped_request_key(fact));
        if (it != request_pages_.end()) pages = it->second;
    }
    if (pages.empty()) return;
    if (!fact.request_id.empty()) request_pages_[scoped_request_key(fact)] = pages;

    const auto matched = radix_tree_.longest_prefix_pages(pages);
    auto matched_pages = slice_pages(pages, 0, matched);
    for (const auto & page : matched_pages) {
        if (l1_.count(page) > 0) {
            touch_page("L1", page);
            if (l2_.count(page) > 0) touch_page("L2", page);
            continue;
        }
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

void HiCacheState::apply_insert_path(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    auto full_pages = pages_for_tokens(fact, fact.full_path_tokens);
    if (full_pages.empty() && !fact.request_id.empty()) {
        auto it = request_pages_.find(scoped_request_key(fact));
        if (it != request_pages_.end()) full_pages = it->second;
    }
    if (full_pages.empty()) return;

    const auto prefix_pages = radix_tree_.longest_prefix_pages(full_pages);
    auto inserted_pages = suffix_pages(full_pages, prefix_pages);
    radix_tree_.insert_path(full_pages);
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

void HiCacheState::apply_prefetch_intent(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    auto pages = suffix_pages_for_prefetch(fact);
    if (pages.empty()) return;
    if (!fact.request_id.empty()) {
        const auto key = scoped_request_key(fact);
        pending_prefetch_pages_[key] = pages;
        prefetch_intent_ts_[key] = fact.ts;
    }
    for (const auto & page : pages) mark_prefetch_planned(fact, summary, transitions, page);
}

void HiCacheState::apply_prefetch_check_point(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    if (fact.request_id.empty()) return;
    const auto key = scoped_request_key(fact);
    auto it = pending_prefetch_pages_.find(key);
    if (it == pending_prefetch_pages_.end() || it->second.empty()) return;
    const auto & pages = it->second;
    if (config_.prefetch_policy == "wait_complete") {
        for (const auto & page : pages) {
            add_resident(fact, summary, transitions, "L3", page);
            add_resident(fact, summary, transitions, "L2", page);
            mark_prefetch_ready(fact, summary, transitions, page);
        }
        enforce_capacity(fact, summary, transitions, "L2");
        return;
    }
    if (config_.prefetch_policy == "best_effort") {
        for (const auto & page : pages) {
            if (prefetch_ready_.count(page) == 0) mark_prefetch_suppressed(fact, summary, transitions, page);
        }
        return;
    }
    if (config_.prefetch_policy != "timeout" || !config_.prefetch_timeout_configured) return;
    auto ts_it = prefetch_intent_ts_.find(key);
    if (ts_it == prefetch_intent_ts_.end() || fact.ts < ts_it->second) return;
    const auto page_size = page_size_for_fact(fact);
    const auto token_count = static_cast<uint64_t>(pages.size()) * page_size;
    double timeout_sec = config_.prefetch_timeout_base_sec + config_.prefetch_timeout_per_ki_token_sec * static_cast<double>(token_count) / 1024.0;
    if (config_.prefetch_timeout_max_sec > 0.0) timeout_sec = std::min(timeout_sec, config_.prefetch_timeout_max_sec);
    const double elapsed_sec = static_cast<double>(fact.ts - ts_it->second) / 1000000.0;
    if (elapsed_sec < timeout_sec) return;
    for (const auto & page : pages) {
        if (prefetch_ready_.count(page) == 0) mark_prefetch_late(fact, summary, transitions, page);
    }
}

void HiCacheState::apply_capacity_request(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    const auto page_size = page_size_for_fact(fact);
    auto pages_to_free = fact.requested_pages;
    if (pages_to_free == 0) pages_to_free = ceil_div(fact.requested_tokens, page_size);
    evict_lru_pages(fact, summary, transitions, "L1", pages_to_free);
    enforce_capacity(fact, summary, transitions, "L1");
    enforce_capacity(fact, summary, transitions, "L2");
}

void HiCacheState::apply_lock_scope_delta(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    summary.lock_state_events++;
    auto pages = pages_for_tokens(fact, fact.logical_path_tokens);
    if (pages.empty()) return;
    const bool increment = fact.lock_direction == "inc" || fact.lock_direction == "increase" || fact.lock_direction == "+";
    for (const auto & page : pages) {
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
        else if (!page_locked_in_any_scope(page)) {
            clear_locked(fact, summary, transitions, page);
        }
    }
}

void HiCacheState::apply_write_policy_hit_counts(const HiCacheFact & fact, const std::vector<std::string> & full_pages, HiCacheSummary & summary,
                                                 std::vector<HiCacheStateTransition> & transitions) {
    if (!target_write_count_enabled()) return;
    const auto threshold = target_write_through_threshold();
    if (threshold == 0) return;
    std::set<std::string> counted;
    for (const auto & page : full_pages) {
        if (page.empty() || !counted.insert(page).second) continue;
        if (l1_.count(page) == 0 && !radix_tree_.contains_page(page)) continue;
        auto before = transition_state_digest();
        const auto key = scoped_page_key(fact, page);
        hit_count_by_scope_page_[key]++;
        record_transition(fact, summary, transitions, "increment_hit_count", "", page, before);
        if (hit_count_by_scope_page_[key] < threshold) continue;
        add_resident(fact, summary, transitions, "L2", page);
        add_resident(fact, summary, transitions, "L3", page);
        mark_backuped(fact, summary, transitions, page);
        clear_dirty(fact, summary, transitions, page);
    }
}

void HiCacheState::add_resident(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                const std::string & tier, const std::string & page) {
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

void HiCacheState::mark_dirty(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                              const std::string & page) {
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

void HiCacheState::evict_lru_pages(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions,
                                   const std::string & tier, uint64_t page_count) {
    auto * pages = tier_set(tier);
    auto * order = touch_order_for_tier(tier);
    if (!pages || !order || page_count == 0) return;
    uint64_t removed = 0;
    std::set<std::string> skipped_locked;
    while (removed < page_count && !order->empty()) {
        auto victim = order->front();
        order->erase(order->begin());
        if (!pages->count(victim)) continue;
        auto victims = radix_tree_.leaf_group_for_page(victim);
        if (victims.empty()) victims = {victim};
        bool locked_group = false;
        for (const auto & page : victims) {
            if (locked_.count(page) > 0) {
                locked_group = true;
                break;
            }
        }
        if (locked_group) {
            skipped_locked.insert(victim);
            continue;
        }
        for (const auto & page : victims) {
            if (!pages->count(page)) continue;
            order->erase(std::remove(order->begin(), order->end(), page), order->end());
            if (dirty_.count(page)) {
                summary.dirty_eviction_events++;
                flush_dirty_page_to_host(fact, summary, transitions, page);
            }
            remove_resident(fact, summary, transitions, tier, page);
            if (tier == "L1")
                mark_evicted(fact, summary, transitions, page);
            else
                clear_evicted(fact, summary, transitions, page);
            removed++;
            if (removed >= page_count) break;
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
    while (pages->size() > capacity && !order->empty()) {
        const auto before = pages->size();
        evict_lru_pages(fact, summary, transitions, tier, pages->size() - capacity);
        if (pages->size() == before) break;
    }
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
        if (left.cache_scope == right.cache_scope && left.seq_no > 0 && right.seq_no > 0 && left.seq_no != right.seq_no) return left.seq_no < right.seq_no;
        if (left.ts != right.ts) return left.ts < right.ts;
        if (left.is_start != right.is_start) return left.is_start && !right.is_start;
        return left.source_event_index < right.source_event_index;
    });

    for (const auto & fact : facts) {
        if (!is_invariant_state_fact(fact)) {
            summary.skipped_non_invariant_events++;
            continue;
        }
        if (!is_known_invariant_role(fact.role)) {
            summary.missing_invariant_facts["unknown_invariant_role"]++;
            continue;
        }
        if (!fact.is_end) continue;

        summary.processed_hicache_events++;
        summary.processed_events_by_role[fact.role]++;
        const auto effective_page_size = config_.page_size > 0 ? config_.page_size : fact.source_page_size;
        if ((fact.role == "request_tokens" || fact.role == "lookup_path" || fact.role == "insert_path") && fact.full_path_tokens.empty() &&
            effective_page_size > 0 && fact.token_count >= effective_page_size) {
            summary.missing_invariant_facts["token_dictionary_or_full_path_span"]++;
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

    if (!summary.missing_invariant_facts.empty()) summary.warnings.push_back("Some HiCache target-state inputs are missing token invariant facts.");
    if (summary.dirty_eviction_events > 0) summary.warnings.push_back("Dirty page eviction triggered modeled writeback state transitions.");
    summary.warnings.push_back("HiCacheModule consumes only invariant_state facts; source_actual/timing_observation/oracle_state are ignored for target state.");
    summary.warnings.push_back("HiCacheModule maintains state only; no DAG mutations are applied.");
    return summary;
}

HiCacheSummary apply_hicache_model(DagGraph & graph, const HiCacheConfig & config) {
    HiCacheStateModel model(config);
    return model.run(graph);
}

} // namespace TraceGraph
