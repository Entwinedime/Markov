#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/source_dag/observations.hpp"
#include "markov/trace_graph/source_dag/prefetch.hpp"
#include "markov/trace_graph/structure/hicache/query.hpp"
#include "source_dag/observation_writer.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <set>
#include <utility>
#include <vector>

namespace markov::trace_graph::source_dag {

void prefetch_query_observations(const core::DagGraph & graph, const CpuCollectiveObservation & collectives, const PrefetchWorkerObservations & workers,
                                 uint64_t begin_us, uint64_t end_us, ObservationWriter & rows) {
    const auto & events = graph.runtime_observations();
    const auto end = [](const core::TraceEvent & event) { return core::checked_add_u64(event.ts, event.dur, "Query observation end overflow"); };
    std::set<size_t> rounds;
    for (const auto & query : workers.queries)
        if (query.issue.empty() && events[query.observation].ts >= begin_us && query.agreement_rounds.size() == 1)
            rounds.insert(query.agreement_rounds.front());

    for (const auto id : rounds) {
        const auto & round = collectives.rounds[id];
        const auto agreement = modules::hicache::observe_cpu_all_reduce_envelope(graph, round);
        if (!agreement.issue.empty()) continue;
        const auto work = structure::hicache::prefetch_query_work(round.members);
        auto group = nlohmann::json::array();
        for (const auto & call : round.calls) {
            const auto & minimum = events[call.observation];
            const auto found = std::ranges::find_if(workers.queries, [&](const auto & query) {
                return query.issue.empty() && query.agreement_rounds == std::vector<size_t>{ id } && events[query.observation].pid == minimum.pid;
            });
            if (found == workers.queries.end() || !found->enqueue || !found->release) break;
            const auto & query = events[found->observation];
            const auto & enqueue = events[*found->enqueue];
            const auto & release = events[*found->release];
            const auto position = measurement_position(query);
            if (!position || query.ts < begin_us || end(release) > end_us) break;

            uint64_t available = enqueue.ts;
            for (const auto & previous : workers.queries) {
                const auto & prior = events[previous.observation];
                if (previous.release && prior.pid == query.pid && prior.tid == query.tid && prior.ts < query.ts)
                    available = std::max(available, end(events[*previous.release]));
            }
            if (available > query.ts || end(query) > minimum.ts || end(minimum) > end(release)) break;

            const auto emit = [&](const structure::OperationWork & operation, uint64_t begin, uint64_t finish) {
                group.push_back({
                    { "position", *position + ":" + operation.timing_scope },
                    { "operation", operation.operation },
                    { "implementation", operation.implementation },
                    { "timing_scope", operation.timing_scope },
                    { "features", operation.features },
                    { "duration_us", graph.cpu_service_cost().duration({ query.pid, query.tid }, begin, finish) }
                });
            };
            const auto & local = work.at(call.rank);
            emit(local.before_sample, available, end(query));
            emit(local.sample_to_min, end(query), minimum.ts);
            emit(local.min_to_apply, end(minimum), end(release));
            // Peer arrival waiting is already represented by dependencies.
            // Measure only the suffix after the last rank has entered.
            emit(local.agreement, end(minimum) - *agreement.calls.at(call.rank).envelope_remainder_us, end(minimum));
        }
        if (group.size() == 4 * round.members.size())
            for (auto & row : group) rows.push_back(std::move(row));
    }
}

} // namespace markov::trace_graph::source_dag
