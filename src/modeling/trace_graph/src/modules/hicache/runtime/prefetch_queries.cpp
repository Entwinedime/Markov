#include "markov/trace_graph/modules/hicache/runtime/prefetch_queries.hpp"
#include "markov/trace_graph/adapters/frameworks/sglang/hicache_observations.hpp"
#include "markov/trace_graph/core/cpu_gap_observation.hpp"
#include "markov/trace_graph/cost/operator_model.hpp"
#include "markov/trace_graph/structure/hicache/query.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace markov::trace_graph::modules::hicache::runtime {


void HiCachePrefetchQueries::bind(core::DagGraph & graph, model::HiCacheModelReplay & replay, cost::OperatorCostProvider & costs,
                                  std::map<size_t, std::vector<size_t>> & facts_at, const structure::hicache::IoCompletionProvider & completion) {
    if (state_) throw std::logic_error("Prefetch queries are already bound");
    state_ = &replay.state();
    worker_ = std::make_unique<model::HiCachePrefetchExecution>(*state_, completion);

    // Candidate creation requires a query even when its eventual cache hit is empty.
    // With no candidates, keep the worker lifecycle but require no query timing.
    if (std::ranges::none_of(replay.facts(), [](const auto & item) { return item.role == HiCacheFactRole::PrefetchCandidateAnchor; })) return;

    const adapters::sglang::HiCacheSourceIndex source(graph);
    const auto collectives = observe_cpu_collectives(source);
    std::map<std::string, int> ranks;
    std::set<int> group_members;
    for (const auto & round : collectives.rounds)
        for (const auto & call : round.calls) {
            const auto [at, added] = ranks.emplace(graph.runtime_observations()[call.observation].pid, call.rank);
            if (!added && at->second != call.rank) throw std::runtime_error("Query process has multiple observed ranks");
            group_members.insert(call.rank);
        }
    std::map<std::string, Query> requests;
    std::vector<core::TraceEvent> points;
    std::vector<std::pair<std::string, int>> keys;
    using EnqueueKey = std::tuple<std::string, std::string, std::string>;
    std::map<EnqueueKey, std::vector<const core::TraceEvent *>> enqueues;
    for (const auto & event : graph.runtime_observations())
        if (event.name == "runtime.hicache.prefetch_enqueue") enqueues[{ event.pid, event.tid, event.arg("request_id") }].push_back(&event);

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
        const auto pending = enqueues.find({ fact.pid, fact.tid, fact.request_id });
        if (pending != enqueues.end())
            for (const auto * event : pending->second)
                if (event->ts >= fact.source_ts - fact.dur && event->ts + event->dur <= fact.source_ts) {
                    ++found;
                    point.ts = event->ts;
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
    }
    for (auto & [request, query] : requests) {
        std::vector<int> participants;
        for (const auto & [rank, fact] : query.facts) participants.push_back(rank);
        if (participants != std::vector<int>(group_members.begin(), group_members.end()))
            throw std::runtime_error("Candidate query must include the source deployment's complete rank group");

        const auto work = structure::hicache::prefetch_query_work(participants);
        model::PrefetchQueueTiming timing;
        for (const auto & [rank, local] : work) {
            timing.cpu.emplace(rank,
                               model::PrefetchQueueCpuTiming{ costs.estimate(local.before_sample).duration_us,
                                                              costs.estimate(local.sample_to_min).duration_us,
                                                              costs.estimate(local.min_to_apply).duration_us });
            timing.agreement.calls.emplace(
                rank,
                CpuCollectiveCallTiming{ .entry_ranks = local.agreement_entries, .envelope_remainder_us = costs.estimate(local.agreement).duration_us });
        }
        query.execution = std::make_unique<model::HiCachePrefetchQueueExecution>(*state_, query.facts, std::move(timing));
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
            worker_->enqueue(fact, future);
        }
    worker_->advance(node, time, future);
}

} // namespace markov::trace_graph::modules::hicache::runtime
