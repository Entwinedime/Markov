#include "markov/trace_graph/modules/hicache/model/prefetch_wait_execution.hpp"
#include <algorithm>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::model {

std::optional<PrefetchWaitTiming> observe_prefetch_stop_timing(const patch::HiCacheSourceDagIndex & source, const CpuCollectiveObservation & rounds,
                                                               const std::map<int, std::vector<PrefetchControlObservation>> & ranks) {
    if (ranks.empty()) return std::nullopt;
    PrefetchWaitTiming result;
    std::optional<size_t> completion;
    for (const auto & [rank, calls] : ranks) {
        if (calls.size() != 1 || calls.front().local_return || !calls.front().stop || calls.front().state_round) return std::nullopt;
        const auto & call = calls.front();
        if (!call.issue.empty() || call.rank != rank || !call.completion_round || call.cpu.empty())
            throw std::invalid_argument("Immediate stop requires complete ranked progress observations");
        if (completion && completion != call.completion_round) throw std::invalid_argument("Immediate stop ranks do not share a completion MIN");
        completion = call.completion_round;
        result.check.cpu.emplace(rank, observe_prefetch_check_cpu(call, &source));
    }
    result.check.completion_min = observe_cpu_collective_timing(source.graph(), rounds.rounds.at(*completion));
    if (!result.check.completion_min.issue.empty() || result.check.completion_min.calls.size() != ranks.size()
        || std::ranges::any_of(result.check.completion_min.calls, [&](const auto & item) { return !ranks.contains(item.first); }))
        throw std::invalid_argument("Immediate stop requires a complete all-rank MIN");
    return result;
}

std::optional<PrefetchWaitTiming> observe_prefetch_wait_timing(const patch::HiCacheSourceDagIndex & source, const CpuCollectiveObservation & rounds,
                                                               const PrefetchWorkerObservations & workers,
                                                               const std::map<int, std::vector<PrefetchControlObservation>> & ranks) {
    if (ranks.empty()) return std::nullopt;
    for (const auto & [rank, calls] : ranks)
        for (const auto & call : calls)
            if (!call.issue.empty() || call.rank != rank || call.cpu.empty())
                throw std::invalid_argument("Wait timing requires valid ranked progress observations");
    if (std::ranges::any_of(ranks, [](const auto & pair) {
            return pair.second.size() < 2 || pair.second.front().local_return || pair.second.front().stop || !pair.second.front().state_round
                   || !pair.second.back().stop;
        }))
        return std::nullopt;
    PrefetchWaitTiming result;
    auto & timing = result.check;
    std::vector<PrefetchControlObservation> first, next;
    for (const auto & [rank, calls] : ranks) {
        if (!calls.back().completion_round) throw std::invalid_argument("Active wait has no terminal completion round");
        first.push_back(calls.front());
        next.push_back(calls[1]);
        auto cpu = observe_prefetch_check_cpu(calls.front(), &source);
        const auto terminal = observe_prefetch_check_cpu(calls.back(), &source);
        cpu.max_to_stop = terminal.max_to_stop;
        cpu.stop_to_min = terminal.stop_to_min;
        cpu.min_to_visible = terminal.min_to_visible;
        timing.cpu[rank] = cpu;
    }
    timing.state_max = observe_cpu_collective_timing(source.graph(), rounds.rounds.at(*first.front().state_round));
    timing.completion_min = observe_cpu_collective_timing(source.graph(), rounds.rounds.at(*ranks.begin()->second.back().completion_round));
    const auto body = observe_prefetch_scheduler_body(source, rounds, workers, first, next);
    if (!body.issue.empty() || !timing.state_max.issue.empty() || !timing.completion_min.issue.empty())
        throw std::runtime_error("Active wait template incomplete: " + body.issue + timing.state_max.issue + timing.completion_min.issue);
    for (const auto & step : body.steps)
        if ((step.action == PrefetchSchedulerAction::WriteCompletion || step.action == PrefetchSchedulerAction::LoadCompletion)
            && step.completion_observations.size() != ranks.size())
            throw std::runtime_error("Wait template has no complete confirmation boundary");
    result.scheduler = prefetch_scheduler_timing(body, source.graph());
    return result;
}

HiCachePrefetchWaitExecution::HiCachePrefetchWaitExecution(HiCacheState & state, std::map<int, HiCacheFact> facts, PrefetchCheckTiming check,
                                                           PrefetchSchedulerTiming body, std::string policy, SchedulerHandler scheduler)
    : state_(state),
      facts_(std::move(facts)),
      check_(std::move(check)),
      body_(std::move(body)),
      policy_(std::move(policy)),
      scheduler_(std::move(scheduler)) {
    if (facts_.empty() || !scheduler_) throw std::invalid_argument("Prefetch waiting requires ranks and explicit scheduler action handling");
}

CpuRankNodes HiCachePrefetchWaitExecution::start(const CpuRankNodes & entries, simulation::FutureDag & future) {
    if (!returned_.empty() || entries.size() != facts_.size()) throw std::logic_error("Prefetch wait entries must be supplied once for every rank");
    for (const auto & [rank, fact] : facts_) {
        returned_[rank] =
            future.append({ .name = "hicache_prefetch_wait_return", .category = "execution_gate" }, simulation::BoundaryOrder::Ordinary, entries.at(rank));
        future.depend(entries.at(rank), returned_.at(rank));
    }
    issue_check(entries, future);
    return returned_;
}

void HiCachePrefetchWaitExecution::issue_check(const CpuRankNodes & entries, simulation::FutureDag & future) {
    auto run = std::make_unique<HiCachePrefetchCheckExecution>(state_, facts_, check_, policy_);
    const auto returned = run->start(entries, future);
    for (const auto & [rank, node] : returned) future.depend(node, returned_.at(rank));
    checks_.push_back({ std::move(run), returned });
}

void HiCachePrefetchWaitExecution::retry(const CpuRankNodes & entries, simulation::FutureDag & future) {
    const auto execution = append_prefetch_scheduler_body(body_, entries, future);
    auto run = std::make_shared<Body>();
    for (const auto & [node, boundary] : execution.actions) actions_.emplace(node, Action{ boundary, run });
    issue_check(execution.returned, future);
}

void HiCachePrefetchWaitExecution::dispatch(const Action & action, uint64_t time, simulation::FutureDag & future) {
    const auto & b = action.boundary;
    const auto kind = body_.steps.at(b.step).action;
    if (kind != PrefetchSchedulerAction::StorageDrain) {
        scheduler_(kind, b, time, future);
        return;
    }
    auto fact = facts_.at(b.rank);
    fact.ts = time;
    auto & samples = action.run->samples[b.step];
    if (!b.apply) {
        samples[b.rank] = state_.prefetch_queue_sizes(fact);
        return;
    }
    if (samples.size() != facts_.size()) throw std::logic_error("Storage drain returned before all rank samples");
    auto common = samples.begin()->second;
    for (const auto & [rank, sample] : samples) {
        common.revoked_operations = std::min(common.revoked_operations, sample.revoked_operations);
        common.backup_acks = std::min(common.backup_acks, sample.backup_acks);
        common.released_pages = std::min(common.released_pages, sample.released_pages);
    }
    state_.drain_prefetch_queues(fact, common);
}

void HiCachePrefetchWaitExecution::advance(size_t node, uint64_t time, simulation::FutureDag & future) {
    if (const auto at = actions_.find(node); at != actions_.end()) {
        const auto action = at->second;
        actions_.erase(at);
        dispatch(action, time, future);
    }
    for (auto at = checks_.begin(); at != checks_.end();) {
        const auto ready = at->execution->advance(node, time, future);
        if (!ready) {
            ++at;
            continue;
        }
        ++at->finished;
        if (!*ready && !at->retried) {
            at->retried = true;
            retry(at->returned, future);
        }
        if (at->finished == facts_.size()) at = checks_.erase(at);
        else ++at;
    }
}

} // namespace markov::trace_graph::modules::hicache::model
