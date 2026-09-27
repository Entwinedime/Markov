#include "markov/trace_graph/modules/hicache/runtime/lifecycle_calls.hpp"
#include "markov/trace_graph/modules/hicache/execution_boundaries.hpp"
#include <algorithm>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::runtime {

void HiCacheLifecycleCalls::bind(core::DagGraph & graph, const model::HiCacheModelReplay & replay, uint64_t begin, uint64_t end) {
    std::vector<core::TraceEvent> points;
    std::vector<Action> actions;
    for (const auto & insert : observe_lifecycle_inserts(graph, replay.facts(), begin, end)) {
        if (!owners_.insert(insert.owner).second) throw std::runtime_error("Lifecycle has multiple insert calls");
        const auto & item = replay.fact(insert.owner);
        const auto append = [&](core::TraceEvent point, Kind kind, uint64_t tokens) {
            point.dur = point.dur_submicro_ns = 0;
            point.name = "hicache.lifecycle_step";
            points.push_back(std::move(point));
            actions.push_back({kind, insert.owner, tokens});
        };
        append(insert.envelope, Kind::Entry, 0);
        for (const auto & step : lifecycle_ready_steps(insert)) append(step.ready, Kind::Ready, step.end_tokens);
        auto returned = insert.envelope;
        const auto ns = returned.ts_submicro_ns + returned.dur_submicro_ns;
        returned.ts += returned.dur + ns / 1000;
        returned.ts_submicro_ns = ns % 1000;
        append(returned, Kind::InsertReturn, item.fact.token_count);
        returned.ts = item.fact.source_ts;
        returned.ts_submicro_ns = 0;
        append(returned, Kind::Return, 0);
    }
    const auto nodes = bind_hicache_control_points(graph, points);
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (!nodes[i]) throw std::runtime_error("Lifecycle step lacks an exact CPU boundary");
        actions_[*nodes[i]].push_back(actions[i]);
    }
}

std::vector<size_t> HiCacheLifecycleCalls::advance(size_t node, uint64_t time, model::HiCacheModelReplay & replay) const {
    std::vector<size_t> updated;
    const auto found = actions_.find(node);
    if (found == actions_.end()) return updated;
    for (const auto & action : found->second) {
        if (action.kind == Kind::Entry)
            replay.apply(action.owner, time, node, model::HiCacheLifecycleExecution::Stepped);
        else if (action.kind == Kind::Return)
            replay.return_lifecycle(action.owner, time, node);
        else
            replay.advance_lifecycle(action.owner, time, node, action.tokens);
        if (std::ranges::find(updated, action.owner) == updated.end()) updated.push_back(action.owner);
    }
    return updated;
}

} // namespace markov::trace_graph::modules::hicache::runtime
