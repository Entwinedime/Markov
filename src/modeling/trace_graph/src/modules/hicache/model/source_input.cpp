#include "modules/hicache/model/source_input.hpp"

#include "core/source_observations.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/modules/hicache/router.hpp"
#include "markov/trace_graph/source_dag/load_admission.hpp"
#include "source_dag/decode_allocation.hpp"

#include <algorithm>
#include <limits>
#include <tuple>
#include <utility>

namespace markov::trace_graph::modules::hicache::model {

HiCacheSourceInput observe_hicache_source_input(const core::SourceObservations & source, HiCachePhaseObservationAudit phases) {
    HiCacheSourceInput result;
    result.phases = std::move(phases);
    HiCacheFactParser parser;
    for (const auto * events : { &source.context_events, &source.prelude_context_events, &source.hicache_fact_events, &source.tail_context_events })
        for (const auto & event : *events) parser.observe_token_dictionaries(event);
    struct Input {
        const core::TraceEvent * event;
        size_t id;
        bool prelude, tail;
    };
    std::vector<Input> input;
    const auto append = [&](const auto & events, bool prelude, bool tail) {
        const auto offset = tail ? hicache_tail_fact_offset(source.hicache_fact_events) : 0;
        for (const auto & event : events) {
            if (!parser.is_hicache_event(event)) continue;
            const auto id =
                prelude ? std::numeric_limits<size_t>::max() - event.index : core::checked_add_u64(offset, event.index, "HiCache tail fact identity overflow");
            input.push_back({ &event, id, prelude, tail });
        }
    };
    append(source.prelude_context_events, true, false);
    append(source.hicache_fact_events, false, false);
    append(source.tail_context_events, false, true);
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
            ++result.missing_facts["unknown_state_model_fact"];
            continue;
        }
        const auto errors = hicache_required_fact_errors(fact, route.role);
        if (!errors.empty()) {
            for (const auto & error : errors) ++result.missing_facts[error];
            continue;
        }
        (item.prelude ? prelude : result.formal).push_back({ std::move(fact), route.role });
    }

    result.prelude_lookups = source_dag::observe_prelude_load_lookups(source.prelude_context_events, prelude);
    result.prelude_decodes = source_dag::observe_prelude_decode_allocations(source.prelude_context_events, prelude);
    result.prelude = std::move(prelude);
    if (!result.formal.empty()) {
        uint64_t begin = result.formal.front().fact.source_ts, end = begin;
        for (const auto & item : result.formal) {
            begin = std::min(begin, item.fact.source_ts);
            end = std::max(end, core::checked_add_u64(item.fact.source_ts, item.fact.dur, "Admission observation window overflow"));
        }
        for (const auto & branch : source_dag::observe_load_branches(source.runtime_observations, result.formal, begin, end))
            result.formal_lookups.push_back(branch.owner);
    }
    return result;
}

} // namespace markov::trace_graph::modules::hicache::model
