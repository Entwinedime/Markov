#include "markov/trace_graph/modules/hicache/model/replay.hpp"
#include "markov/trace_graph/core/numeric.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <tuple>

namespace markov::trace_graph::modules::hicache::model {

HiCacheModelReplay::HiCacheModelReplay(const core::DagGraph & graph, const frontend::HiCacheConfig & config, bool execution_prefetch_control,
                                       const HiCacheFactClock & projected_clock)
    : state_(config),
      execution_prefetch_control_(execution_prefetch_control) {
    if (execution_prefetch_control && projected_clock)
        throw std::invalid_argument("Execution-driven facts take their time at apply(), not from a precomputed clock");
    result_.io_cost_model = config.io_cost;
    result_.phase_cost_model = config.phase_cost;
    if (!config.kv_bytes_per_page) result_.effect_decisions.missing_facts["kv_geometry"] = 1;
    if (!config.io_cost.storage_batch_pages) result_.effect_decisions.missing_facts["io_service_model"] = 1;
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
    // Identity is a property of the input, not the order in which target lanes run.
    for (const auto & item : facts_) state_.register_effect_identity(item.fact);
    for (const auto & item : prelude)
        if (item.role == HiCacheFactRole::CacheExtendInput) state_.register_prefetch_control_boundary(item.fact);
    for (const auto & item : prelude) state_.apply_fact(item.fact, item.role, false);
    if (!graph.prelude_context_events().empty()) state_.finalize();
    if (!graph.prelude_context_events().empty() || execution_prefetch_control) state_.begin_formal_window(execution_prefetch_control);
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

void HiCacheModelReplay::apply(size_t fact_id, uint64_t timestamp_us, std::optional<size_t> execution_node, HiCacheLifecycleExecution lifecycle) {
    auto & item = facts_.at(fact_positions_.at(fact_id));
    if (finished_ || item.started || (last_time_ && timestamp_us < *last_time_))
        throw std::logic_error("HiCache facts must execute once in nondecreasing time before finish");
    item.fact.ts = timestamp_us;
    item.fact.execution_anchor_node_id = execution_node;
    state_.apply_fact(item.fact, item.role, true, lifecycle);
    item.started = true;
    item.consumed = !state_.allocation_pending(item.fact) && !state_.lifecycle_return_pending(item.fact);
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
