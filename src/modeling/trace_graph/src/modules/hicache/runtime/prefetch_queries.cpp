#include "markov/trace_graph/modules/hicache/runtime/prefetch_queries.hpp"
#include "markov/trace_graph/core/cpu_gap_observation.hpp"
#include "markov/trace_graph/modules/hicache/model/prefetch_wait_calibration.hpp"
#include <algorithm>
#include <set>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::runtime {

PrefetchQueryTemplate observe_prefetch_query_template(const core::DagGraph & graph, const CpuCollectiveObservation & collectives,
                                                      const PrefetchWorkerObservations & workers, uint64_t begin_us, uint64_t end_us) {
    const auto & events = graph.runtime_observations();
    std::set<size_t> unique_rounds;
    for (const auto & query : workers.queries)
        if (query.issue.empty() && events[query.observation].ts >= begin_us && query.agreement_rounds.size() == 1)
            unique_rounds.insert(query.agreement_rounds.front());
    std::vector<size_t> rounds(unique_rounds.begin(), unique_rounds.end());
    std::ranges::stable_sort(rounds, {}, [&](size_t id) { return events[collectives.rounds[id].calls.front().observation].ts; });
    for (const auto id : rounds) {
        const auto & round = collectives.rounds[id];
        PrefetchQueryTemplate result;
        result.timing.agreement = observe_cpu_all_reduce_envelope(graph, round);
        if (!result.timing.agreement.issue.empty()) continue;
        for (const auto & call : round.calls) {
            const auto & min = events[call.observation];
            const auto found = std::ranges::find_if(workers.queries, [&](const auto & query) {
                return query.issue.empty() && query.agreement_rounds == std::vector<size_t>{ id } && events[query.observation].pid == min.pid;
            });
            if (found == workers.queries.end() || !found->enqueue || !found->release) break;
            const auto & query = events[found->observation];
            const auto & enqueue = events[*found->enqueue];
            const auto & release = events[*found->release];
            if (query.ts < begin_us || release.ts + release.dur > end_us) break;
            uint64_t available = enqueue.ts;
            for (const auto & previous : workers.queries) {
                const auto & prior = events[previous.observation];
                if (previous.release && prior.pid == query.pid && prior.tid == query.tid && prior.ts < query.ts)
                    available = std::max(available, events[*previous.release].ts + events[*previous.release].dur);
            }
            if (available > query.ts || query.ts + query.dur > min.ts || min.ts + min.dur > release.ts + release.dur) break;
            result.timing.cpu[call.rank] = { query.ts + query.dur - available, min.ts - query.ts - query.dur, release.ts + release.dur - min.ts - min.dur };
            result.observations[call.rank] = found->observation;
        }
        if (result.timing.cpu.size() == round.members.size()) return result;
    }
    return { .issue = "base_window_has_no_complete_query_template" };
}

void HiCachePrefetchQueries::bind(core::DagGraph & graph, model::HiCacheModelReplay & replay, const frontend::HiCacheConfig & config,
                                  std::map<size_t, std::vector<size_t>> & facts_at, uint64_t begin_us, uint64_t end_us, ReturnObserver observer) {
    if (state_) throw std::logic_error("Prefetch queries are already bound");
    const patch::HiCacheSourceDagIndex source(graph);
    const auto collectives = observe_cpu_collectives(source);
    template_ = observe_prefetch_query_template(graph, collectives, observe_prefetch_workers(source, collectives), begin_us, end_us);
    if (!template_.issue.empty() && !config.prefetch_query_calibration.empty()) {
        const auto calibration = model::read_prefetch_query_calibration(config.prefetch_query_calibration);
        template_ = {.timing = calibration.timing, .calibration_manifest = calibration.source_manifest};
    }
    if (!template_.issue.empty()) throw std::runtime_error(template_.issue);
    state_ = &replay.state();
    observer_ = std::move(observer);
    worker_ = std::make_unique<model::HiCachePrefetchExecution>(*state_, config);
    std::map<std::string, int> ranks;
    for (const auto & round : collectives.rounds)
        for (const auto & call : round.calls) {
            const auto [at, added] = ranks.emplace(graph.runtime_observations()[call.observation].pid, call.rank);
            if (!added && at->second != call.rank) throw std::runtime_error("Query process has multiple observed ranks");
        }
    std::map<std::string, Query> requests;
    std::vector<core::TraceEvent> points;
    std::vector<std::pair<std::string, int>> keys;
    for (const auto & item : replay.facts()) {
        if (item.role != HiCacheFactRole::PrefetchCandidateAnchor) continue;
        const auto & fact = item.fact;
        const auto rank = ranks.at(fact.pid);
        auto & query = requests[fact.request_id];
        if (!query.facts.emplace(rank, fact).second) throw std::runtime_error("Multiple candidate attempts need separate query identities");
        core::TraceEvent point;
        point.name = "prefetch_candidate_submission";
        point.pid = fact.pid;
        point.tid = fact.tid;
        point.ts = fact.source_ts;
        size_t found = 0;
        for (const auto & event : graph.runtime_observations())
            if (event.name == "runtime.hicache.prefetch_enqueue" && event.pid == fact.pid && event.tid == fact.tid && event.arg("request_id") == fact.request_id
                && event.ts >= fact.source_ts - fact.dur && event.ts + event.dur <= fact.source_ts) {
                ++found;
                point.ts = event.ts;
            }
        if (found > 1) throw std::runtime_error("Candidate has multiple enqueue observations");
        points.push_back(std::move(point));
        keys.emplace_back(fact.request_id, rank);
    }
    const auto bound = core::insert_cpu_gap_points(graph, points);
    for (size_t i = 0; i < bound.size(); ++i) {
        const auto & [request, rank] = keys[i];
        auto & query = requests.at(request);
        const auto & fact = query.facts.at(rank);
        std::optional<size_t> previous;
        for (auto & [node, ids] : facts_at)
            if (std::erase(ids, fact.source_node_id)) previous = node;
        if (!previous) throw std::runtime_error("Candidate was not bound");
        const auto node = bound[i] ? bound[i] : points[i].ts == fact.source_ts ? previous : std::nullopt;
        if (!node || graph.node(*node).duration) throw std::runtime_error("Enqueue needs an exact zero-cost execution boundary");
        facts_at[*node].push_back(fact.source_node_id);
        query.entries[rank] = *node;
        entries_.push_back({ request, rank, fact.source_ts, points[i].ts });
    }
    for (auto & [request, query] : requests) {
        if (query.facts.size() != template_.timing.cpu.size()
            || std::ranges::any_of(query.facts, [&](const auto & item) { return !template_.timing.cpu.contains(item.first); }))
            throw std::runtime_error("Candidate group does not match query communication membership");
        query.execution = std::make_unique<model::HiCachePrefetchQueueExecution>(*state_, query.facts, template_.timing, model::PrefetchQueueAction::Query);
        queries_.push_back(std::move(query));
    }
    std::ranges::sort(queries_, {}, [](const auto & query) { return query.facts.begin()->second.source_ts; });
}

void HiCachePrefetchQueries::start(simulation::FutureDag & future) {
    CpuRankNodes previous;
    for (auto & query : queries_) {
        // Query FIFO never blocks candidate creation or the foreground thread.
        // Payload completion belongs to the separate worker queue.
        CpuRankNodes entries;
        for (const auto & [rank, submitted] : query.entries) {
            entries[rank] =
                future.append({ .name = "prefetch query FIFO entry", .category = "execution_gate" }, simulation::BoundaryOrder::Ordinary, submitted);
            future.depend(submitted, entries.at(rank));
            if (previous.contains(rank)) future.depend(previous.at(rank), entries.at(rank));
        }
        previous = query.execution->start(entries, future);
    }
}

void HiCachePrefetchQueries::advance(size_t node, uint64_t time, simulation::FutureDag & future) {
    for (auto & query : queries_)
        if (const auto rank = query.execution->advance(node, time, future)) {
            const auto & fact = query.facts.at(*rank);
            const auto * operation = state_->prefetch_candidate_operation(fact);
            const auto payload = worker_->enqueue(fact, future);
            if (observer_) observer_({ fact.request_id, *rank, time, operation ? operation->hit_pages.size() : 0, operation != nullptr, payload.has_value() });
        }
    worker_->advance(node, time, future);
}

} // namespace markov::trace_graph::modules::hicache::runtime
