#include "markov/trace_graph/modules/hicache/execution_boundaries.hpp"
#include "markov/trace_graph/core/cpu_gap_observation.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/modules/hicache/router.hpp"
#include "markov/trace_graph/modules/hicache/model/replay.hpp"

#include <tuple>

namespace markov::trace_graph::modules::hicache {

HiCacheExecutionBoundaries bind_hicache_execution_boundaries(core::DagGraph & graph, std::span<const model::HiCacheReplayFact> prepared) {
    HiCacheExecutionBoundaries result;
    HiCacheFactParser parser;
    for (const auto * events : { &graph.context_events(), &graph.prelude_context_events(), &graph.hicache_fact_events(), &graph.tail_context_events() })
        for (const auto & event : *events) parser.observe_token_dictionaries(event);
    using Point = std::tuple<std::string, std::string, uint64_t>;
    std::map<Point, std::vector<size_t>> existing;
    for (const auto & node : graph.nodes()) {
        if (!node.active || !node.is_cpu || node.duration != 0) continue;
        const auto & event = graph.event_for_node(node.id);
        if (event.cat == "observed_boundary" && event.name == "hicache.state_boundary.point") existing[{ event.pid, event.tid, event.ts }].push_back(node.id);
    }
    std::vector<core::TraceEvent> points;
    const auto append_fact = [&](const HiCacheFact & fact) {
        const auto fact_id = fact.source_node_id;
        if (const auto found = existing.find({ fact.pid, fact.tid, fact.source_ts }); found != existing.end()) {
            if (found->second.size() == 1) result.fact_nodes[fact_id] = found->second.front();
            else result.issues[fact_id] = { "nonunique_state_execution_point" };
            return;
        }
        core::TraceEvent point;
        point.index = fact_id;
        point.name = "hicache.state_boundary";
        point.pid = fact.pid;
        point.tid = fact.tid;
        point.ts = fact.source_ts;
        points.push_back(std::move(point));
    };
    const auto append = [&](const core::TraceEvent & event, size_t fact_id, bool tail) {
        if (!parser.is_hicache_event(event)) return;
        const auto fact = parser.parse(fact_id, event);
        const auto route = route_hicache_fact(fact);
        if (!route.model_fact || (tail && route.role != HiCacheFactRole::CacheLifecycleCommit)) return;
        auto errors = hicache_required_fact_errors(fact, route.known_role ? route.role : HiCacheFactRole::Unknown);
        if (!errors.empty()) {
            result.issues[fact_id] = std::move(errors);
            return;
        }
        append_fact(fact);
    };
    if (!prepared.empty()) {
        // Replay has validated inputs and joined derived decode actions. Bind
        // those same identities instead of independently rebuilding a fact list.
        for (const auto & item : prepared) append_fact(item.fact);
    } else {
        for (const auto & event : graph.hicache_fact_events()) append(event, event.index, false);
        const auto tail_offset = hicache_tail_fact_offset(graph.hicache_fact_events());
        for (const auto & event : graph.tail_context_events())
            append(event, core::checked_add_u64(tail_offset, event.index, "HiCache tail fact identity overflow"), true);
    }
    const auto nodes = core::insert_cpu_gap_points(graph, points);
    for (size_t i = 0; i < points.size(); ++i) {
        if (nodes[i]) result.fact_nodes[points[i].index] = *nodes[i];
        else result.issues[points[i].index] = { "no_unique_unchanged_cpu_gap" };
    }
    return result;
}

} // namespace markov::trace_graph::modules::hicache
