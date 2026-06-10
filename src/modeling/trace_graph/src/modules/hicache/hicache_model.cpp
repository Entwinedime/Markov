#include "trace_graph/modules/hicache/hicache_model.hpp"

#include "trace_graph/core/dag_graph.hpp"
#include "trace_graph/modules/hicache/hicache_router.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace TraceGraph {

namespace {

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::vector<std::string> sorted_vector(const std::set<std::string> & values) { return {values.begin(), values.end()}; }

std::vector<std::string> slice_pages(const std::vector<std::string> & pages, size_t begin, size_t end) {
    if (begin >= pages.size() || begin >= end) return {};
    end = std::min(end, pages.size());
    return {pages.begin() + static_cast<long>(begin), pages.begin() + static_cast<long>(end)};
}

std::vector<std::string> suffix_pages(const std::vector<std::string> & pages, size_t begin) { return slice_pages(pages, begin, pages.size()); }

} // namespace

std::string HiCacheState::digest() const { return state_index_.digest(); }

HiCacheState::HiCacheState(HiCacheConfig config) : config_(std::move(config)), target_pager_(config_) {}

uint64_t HiCacheState::page_size_for_fact(const HiCacheFact & fact) const { return target_pager_.page_size_for_fact(fact); }

std::vector<std::string> HiCacheState::pages_for_tokens(const HiCacheFact & fact, const HiCacheTokenPath & tokens) const {
    return target_pager_.pages_for_tokens(fact, tokens);
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

std::vector<HiCacheStateTransition> HiCacheState::apply_fact(const HiCacheFact & fact, HiCacheSummary & summary) {
    std::vector<HiCacheStateTransition> transitions;
    if (fact.role == "request_tokens") {
        apply_request_tokens(fact);
        return transitions;
    }
    if (fact.role == "cache_config_observed") { return transitions; }
    if (fact.role == "lookup_path") {
        apply_lookup_path(fact, summary, transitions);
        return transitions;
    }
    if (fact.role == "insert_path") {
        apply_insert_path(fact, summary, transitions);
        return transitions;
    }
    if (fact.role == "prefetch_decision") {
        apply_prefetch_decision(fact, summary, transitions);
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

void HiCacheState::apply_request_tokens(const HiCacheFact & fact) { token_store_.set_request_tokens(fact, fact.full_path_tokens); }

void HiCacheState::apply_lookup_path(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    auto tokens = fact.full_path_tokens;
    if (tokens.empty()) tokens = token_store_.request_tokens(fact);
    auto pages = pages_for_tokens(fact, tokens);
    if (pages.empty()) return;
    token_store_.set_request_tokens(fact, tokens);

    const auto matched = radix_tree_.longest_prefix_pages(tokens, page_size_for_fact(fact));
    auto matched_pages = slice_pages(pages, 0, matched);
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

void HiCacheState::apply_insert_path(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    auto tokens = fact.full_path_tokens;
    if (tokens.empty()) tokens = token_store_.request_tokens(fact);
    auto full_pages = pages_for_tokens(fact, tokens);
    if (full_pages.empty()) return;
    token_store_.set_request_tokens(fact, tokens);

    const auto prefix_pages = radix_tree_.longest_prefix_pages(tokens, page_size_for_fact(fact));
    auto inserted_pages = suffix_pages(full_pages, prefix_pages);
    radix_tree_.insert_path(tokens, full_pages);
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
    auto tokens = fact.full_path_tokens;
    if (tokens.empty()) tokens = token_store_.request_tokens(fact);
    auto full_pages = pages_for_tokens(fact, tokens);
    if (full_pages.empty()) return;
    token_store_.set_request_tokens(fact, tokens);

    const auto prefix_pages = radix_tree_.longest_prefix_pages(tokens, page_size_for_fact(fact));
    auto pages = suffix_pages(full_pages, prefix_pages);
    if (pages.empty()) return;
    if (!fact.request_id.empty()) {
        const auto key = scoped_request_key(fact);
        pending_prefetch_pages_[key] = pages;
        prefetch_decision_ts_[key] = fact.ts;
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
        pending_prefetch_pages_.erase(it);
        return;
    }
    if (config_.prefetch_policy == "best_effort") {
        for (const auto & page : pages) {
            if (state_index_.prefetch_ready().count(page) == 0) mark_prefetch_suppressed(fact, summary, transitions, page);
        }
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
}

void HiCacheState::apply_capacity_request(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    // A capacity request gives the requested allocation size, not the concrete
    // victim set. Treat it as a capacity checkpoint and let normal capacity
    // enforcement evict only when the modeled tier is already over capacity.
    enforce_capacity(fact, summary, transitions, "L1");
    enforce_capacity(fact, summary, transitions, "L2");
}

void HiCacheState::apply_lock_scope_delta(const HiCacheFact & fact, HiCacheSummary & summary, std::vector<HiCacheStateTransition> & transitions) {
    summary.lock_state_events++;
    auto pages = pages_for_tokens(fact, fact.logical_path_tokens);
    if (pages.empty()) return;
    const bool increment = fact.lock_direction == "inc" || fact.lock_direction == "increase" || fact.lock_direction == "+";
    for (const auto & page : pages) {
        if (increment) {
            state_index_.increment_lock_count(fact.cache_scope, page);
            mark_locked(fact, summary, transitions, page);
            continue;
        }
        if (state_index_.decrement_lock_count(fact.cache_scope, page)) { clear_locked(fact, summary, transitions, page); }
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
        if (state_index_.l1().count(page) == 0 && !radix_tree_.contains_page(page)) continue;
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
            if (state_index_.locked().count(page) > 0) {
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
            if (state_index_.dirty().count(page)) {
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
        const auto route = route_hicache_fact(fact);
        if (!route.state_model_input) {
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
