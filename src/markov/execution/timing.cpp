#include "markov/execution/timing.hpp"

#include <algorithm>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#include "markov/graph/graph.hpp"

namespace markov {

GraphTiming GraphTiming::conditioned(const CoarseGraph & graph, std::vector<ServiceDuration> work_ns, std::vector<BoundaryTime> boundaries,
                                     std::optional<TimeWindow> unmeasured_completion_window) {
    if (work_ns.size() != graph.work.size()) throw std::invalid_argument("graph timing requires one service declaration per work interval");
    GraphTiming result{ std::move(work_ns), std::move(boundaries) };
    if (std::ranges::all_of(result.work_ns, [](const auto & value) { return value.has_value(); })) return result;

    std::vector<std::vector<std::size_t>> next(graph.events.size());
    std::vector<std::size_t> pending(graph.events.size());
    std::vector<std::optional<std::int64_t>> upper(graph.events.size());
    std::set<std::size_t> bounded;
    for (const auto & boundary : result.boundaries) {
        upper.at(boundary.event) = boundary.window.end_ns;
        bounded.insert(boundary.event);
    }
    for (const auto & edge : graph.edges) {
        next[edge.source].push_back(edge.target);
        ++pending[edge.target];
    }
    for (const auto & work : graph.work) {
        next[work.start].push_back(work.finish);
        ++pending[work.finish];
    }
    std::vector<std::size_t> ordered;
    for (std::size_t event = 0; event < pending.size(); ++event) {
        if (pending[event] == 0) ordered.push_back(event);
    }
    for (std::size_t position = 0; position < ordered.size(); ++position) {
        for (const auto target : next[ordered[position]]) {
            if (--pending[target] == 0) ordered.push_back(target);
        }
    }

    // Named conditions bound their actual causal ancestors, never another stream's unrelated work.
    for (const auto event : std::views::reverse(ordered)) {
        for (const auto target : next[event]) {
            if (upper[target] && (!upper[event] || *upper[target] < *upper[event])) upper[event] = upper[target];
        }
    }
    for (std::size_t index = 0; index < graph.work.size(); ++index) {
        const auto & work = graph.work[index];
        if (result.work_ns[index]) continue;
        if (bounded.contains(work.finish)) continue;
        if (!upper[work.finish] && !unmeasured_completion_window)
            throw std::invalid_argument("unknown work has no causal or declared observation bound at " + graph.scopes[work.scope].info.site);

        // A synchronized observation can enclose private async tails without prescribing an internal event time.
        auto window = unmeasured_completion_window ? *unmeasured_completion_window : TimeWindow{ 0, *upper[work.finish] };
        if (upper[work.finish]) window.end_ns = std::min(window.end_ns, *upper[work.finish]);
        if (window.begin_ns < 0 || window.begin_ns > window.end_ns)
            throw std::invalid_argument("unmeasured completion window conflicts with causal bounds at " + graph.scopes[work.scope].info.site);
        result.boundaries.push_back({ work.finish, window });
        bounded.insert(work.finish);
    }
    return result;
}

} // namespace markov
