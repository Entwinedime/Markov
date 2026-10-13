#include "markov/trace_graph/modules/hicache/model/replay.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/source_dag/decode_allocation.hpp"
#include "markov/trace_graph/source_dag/load_admission.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace markov::trace_graph::modules::hicache::model {

HiCacheModelReplay::HiCacheModelReplay(const core::DagGraph & graph, const frontend::HiCacheRules & config, structure::hicache::IoCompletionProvider completion,
                                       uint64_t storage_batch_pages, frontend::HiCacheIoResourceLanesConfig resource_lanes, bool execution_prefetch_control,
                                       const HiCacheFactClock & projected_clock)
    : state_(config, std::move(completion), storage_batch_pages, std::move(resource_lanes)),
      execution_prefetch_control_(execution_prefetch_control) {
    if (execution_prefetch_control && projected_clock)
        throw std::invalid_argument("Execution-driven facts take their time at apply(), not from a precomputed clock");
    if (!config.kv_bytes_per_page) result_.effect_decisions.missing_facts["kv_geometry"] = 1;
    if (!storage_batch_pages) result_.effect_decisions.missing_facts["storage_batch_geometry"] = 1;
    if (!result_.effect_decisions.missing_facts.empty()) {
        result_.effect_decisions.status = "needs_calibration";
        result_.phase_work.blockers = result_.effect_decisions.missing_facts;
        return;
    }
    source_phases_ = observe_hicache_phases(graph);
    for (size_t i = 0; i < source_phases_.observations.size(); ++i) {
        const auto & observation = source_phases_.observations[i];
        if (observation.request_ids.size() != 1) {
            ++result_.phase_work.blockers["source_phase_batch_not_single_request"];
            continue;
        }

        if (!source_phase_positions_.emplace(std::pair{ observation.pid, observation.request_ids.front() }, i).second)
            ++result_.phase_work.blockers["duplicate_source_phase_request"];
    }
    if (!source_phases_.ready()) ++result_.phase_work.blockers["source_phase_observation_not_ready"];

    HiCacheFactParser parser;
    for (const auto * events : { &graph.context_events(), &graph.prelude_context_events(), &graph.hicache_fact_events(), &graph.tail_context_events() })
        for (const auto & event : *events) parser.observe_token_dictionaries(event);
    struct Input {
        const core::TraceEvent * event;
        size_t id;
        bool prelude, tail;
    };
    std::vector<Input> input;
    const auto append = [&](const auto & events, bool prelude, bool tail) {
        const auto offset = tail ? hicache_tail_fact_offset(graph.hicache_fact_events()) : 0;
        for (const auto & event : events) {
            if (!parser.is_hicache_event(event)) continue;
            const auto id =
                prelude ? std::numeric_limits<size_t>::max() - event.index : core::checked_add_u64(offset, event.index, "HiCache tail fact identity overflow");
            input.push_back({ &event, id, prelude, tail });
        }
    };
    append(graph.prelude_context_events(), true, false);
    append(graph.hicache_fact_events(), false, false);
    append(graph.tail_context_events(), false, true);
    std::ranges::sort(input, [](const auto & a, const auto & b) {
        const auto & x = *a.event;
        const auto & y = *b.event;
        return std::tuple{ !a.prelude, hicache_fact_boundary_timestamp(x), x.pid, x.tid, x.name, x.index, a.id }
               < std::tuple{ !b.prelude, hicache_fact_boundary_timestamp(y), y.pid, y.tid, y.name, y.index, b.id };
    });
    std::vector<HiCacheReplayFact> prelude;
    for (const auto & item : input) {
        auto fact = parser.parse(item.id, *item.event);
        const auto route = route_hicache_fact(fact);
        if (!route.model_fact || (item.tail && route.role != HiCacheFactRole::CacheLifecycleCommit)) continue;
        if (!route.known_role) {
            ++result_.effect_decisions.missing_facts["unknown_state_model_fact"];
            continue;
        }
        const auto errors = hicache_required_fact_errors(fact, route.role);
        if (!errors.empty()) {
            for (const auto & error : errors) ++result_.effect_decisions.missing_facts[error];
            continue;
        }
        (item.prelude ? prelude : facts_).push_back({ std::move(fact), route.role });
    }
    for (const auto lookup : source_dag::observe_prelude_load_lookups(graph.prelude_context_events(), prelude)) state_.register_load_admission_lookup(lookup);

    const auto check_admission_coverage = [&](const auto & inputs, const char * missing) {
        std::set<std::pair<std::string, std::string>> admitted_requests;
        for (const auto & item : inputs)
            if (item.role == HiCacheFactRole::CacheLookupInput && state_.has_load_admission(item.fact.source_node_id))
                admitted_requests.emplace(item.fact.pid, item.fact.request_id);

        // An absent probe is not evidence of a no-load target branch. Serial
        // requests need an opportunity irrespective of the source outcome.
        for (const auto & item : inputs)
            if (item.role == HiCacheFactRole::CacheExtendInput)
                for (const auto & path : item.fact.batch_paths)
                    if (!admitted_requests.contains({ item.fact.pid, path.request_id })) ++result_.phase_work.blockers[missing];
    };
    check_admission_coverage(prelude, "source_prelude_load_admission_boundary_missing");

    if (!facts_.empty()) {
        uint64_t begin = facts_.front().fact.source_ts, end = begin;
        for (const auto & item : facts_) {
            begin = std::min(begin, item.fact.source_ts);
            end = std::max(end, core::checked_add_u64(item.fact.source_ts, item.fact.dur, "Admission observation window overflow"));
        }
        const auto branches = source_dag::observe_load_branches(graph.runtime_observations(), facts_, begin, end);
        for (const auto & branch : branches) state_.register_load_admission_lookup(branch.owner);
        check_admission_coverage(facts_, "source_load_admission_boundary_missing");
    }

    // Identity is a property of the input, not the order in which target lanes run.
    for (const auto & item : facts_) state_.register_effect_identity(item.fact);
    for (const auto & item : prelude)
        if (item.role == HiCacheFactRole::CacheExtendInput) state_.register_prefetch_control_boundary(item.fact);
    for (const auto & item : prelude) state_.apply_fact(item.fact, item.role, false);
    // Backend descriptors and other diagnostic context do not initialize
    // cache state. Only accepted prelude facts need a state/window transition.
    if (!prelude.empty()) state_.finalize();
    if (!prelude.empty() || execution_prefetch_control) state_.begin_formal_window(execution_prefetch_control);
    if (projected_clock) {
        for (auto & item : facts_) item.fact.ts = projected_clock(item.fact);
        std::ranges::stable_sort(facts_, {}, [](const auto & item) { return item.fact.ts; });
    }
    for (size_t i = 0; i < facts_.size(); ++i) {
        const auto & item = facts_[i];
        if (!fact_positions_.emplace(item.fact.source_node_id, i).second) throw std::invalid_argument("HiCache formal facts have duplicate identities");
        if (!execution_prefetch_control && item.role == HiCacheFactRole::CacheExtendInput) state_.register_prefetch_control_boundary(item.fact);
    }
}

void HiCacheModelReplay::prepare_decode_allocations(const core::DagGraph & source_graph, uint64_t begin, uint64_t end) {
    if (last_time_ || finished_ || !execution_prefetch_control_) throw std::logic_error("Decode observations must be prepared before formal execution");
    auto derived = source_dag::observe_decode_allocations(source_graph, facts_, source_phases_, begin, end);
    for (auto & item : derived) {
        state_.register_effect_identity(item.fact);
        facts_.push_back(std::move(item));
    }

    std::ranges::stable_sort(facts_, {}, [](const auto & item) { return item.fact.source_ts; });
    fact_positions_.clear();
    for (size_t i = 0; i < facts_.size(); ++i) fact_positions_.emplace(facts_[i].fact.source_node_id, i);
}

void HiCacheModelReplay::apply(size_t fact_id, uint64_t timestamp_us, std::optional<size_t> execution_node, HiCacheLifecycleExecution lifecycle) {
    auto & item = facts_.at(fact_positions_.at(fact_id));
    if (finished_ || item.started || (last_time_ && timestamp_us < *last_time_))
        throw std::logic_error("HiCache facts must execute once in nondecreasing time before finish");
    item.fact.ts = timestamp_us;
    item.fact.execution_anchor_node_id = execution_node;
    state_.apply_fact(item.fact, item.role, true, lifecycle);
    item.started = true;
    const bool awaits_admission = execution_prefetch_control_ && item.role == HiCacheFactRole::CacheLookupInput && state_.has_load_admission(fact_id);
    item.consumed = !awaits_admission && !state_.allocation_pending(item.fact) && !state_.lifecycle_return_pending(item.fact);
    last_time_ = timestamp_us;
}

void HiCacheModelReplay::admit_load(size_t fact_id, uint64_t timestamp_us, size_t execution_node) {
    auto & item = facts_.at(fact_positions_.at(fact_id));
    if (finished_ || !execution_prefetch_control_ || !item.started || item.consumed || !state_.has_load_admission(fact_id)
        || (last_time_ && timestamp_us < *last_time_))
        throw std::logic_error("Load admission requires its unfinished lookup and a forward execution clock");

    item.fact.ts = timestamp_us;
    item.fact.execution_anchor_node_id = execution_node;
    state_.admit_load(item.fact);
    item.consumed = !state_.allocation_pending(item.fact);
    last_time_ = timestamp_us;
}

HiCacheReplayFact & HiCacheModelReplay::continue_lifecycle(size_t fact_id, uint64_t timestamp_us, size_t execution_node) {
    auto & item = facts_.at(fact_positions_.at(fact_id));
    if (finished_ || !item.started || item.consumed || !state_.lifecycle_return_pending(item.fact) || (last_time_ && timestamp_us < *last_time_))
        throw std::logic_error("Lifecycle continuation requires a pending return and a forward execution clock");
    item.fact.ts = timestamp_us;
    item.fact.execution_anchor_node_id = execution_node;
    return item;
}

void HiCacheModelReplay::advance_lifecycle(size_t fact_id, uint64_t timestamp_us, size_t execution_node, uint64_t ready_tokens) {
    auto & item = continue_lifecycle(fact_id, timestamp_us, execution_node);
    state_.advance_lifecycle_insert(item.fact, ready_tokens);
    last_time_ = timestamp_us;
}

void HiCacheModelReplay::return_lifecycle(size_t fact_id, uint64_t timestamp_us, size_t execution_node) {
    auto & item = continue_lifecycle(fact_id, timestamp_us, execution_node);
    state_.complete_lifecycle_return(item.fact);
    item.consumed = true;
    last_time_ = timestamp_us;
}

void HiCacheModelReplay::resume(size_t fact_id, uint64_t timestamp_us, size_t execution_node) {
    auto & item = facts_.at(fact_positions_.at(fact_id));
    if (finished_ || !item.started || item.consumed || (last_time_ && timestamp_us < *last_time_))
        throw std::logic_error("Allocation continuation requires an unfinished fact and a forward execution clock");
    item.fact.ts = timestamp_us;
    item.fact.execution_anchor_node_id = execution_node;
    state_.resume_allocation(item.fact);
    item.consumed = true;
    last_time_ = timestamp_us;
}

} // namespace markov::trace_graph::modules::hicache::model
