#include "markov/trace_graph/modules/hicache/model/prefetch_wait_execution.hpp"
#include <algorithm>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::model {

HiCachePrefetchWaitExecution::HiCachePrefetchWaitExecution(HiCacheState & state, std::map<int, HiCacheFact> facts, SchedulerCosts scheduler_costs,
                                                           std::string policy, PrefetchCheckLocalCost local_cost, PrefetchCheckCollectiveCost collective_cost,
                                                           SchedulerHandler scheduler)
    : state_(state),
      facts_(std::move(facts)),
      scheduler_costs_(std::move(scheduler_costs)),
      policy_(std::move(policy)),
      local_cost_(std::move(local_cost)),
      collective_cost_(std::move(collective_cost)),
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
    auto run = std::make_unique<HiCachePrefetchCheckExecution>(state_, facts_, policy_, local_cost_, collective_cost_);
    const auto returned = run->start(entries, future);
    for (const auto & [rank, node] : returned) future.depend(node, returned_.at(rank));
    checks_.push_back({ std::move(run), returned });
}

void HiCachePrefetchWaitExecution::retry(const CpuRankNodes & entries, simulation::FutureDag & future) {
    const auto execution = append_prefetch_scheduler_body(scheduler_costs_(), entries, future);
    auto run = std::make_shared<Body>();
    for (const auto & [node, boundary] : execution.actions) actions_.emplace(node, Action{ boundary, run });
    issue_check(execution.returned, future);
}

void HiCachePrefetchWaitExecution::dispatch(const Action & action, uint64_t time, simulation::FutureDag & future) {
    const auto & b = action.boundary;
    const auto kind = b.action;
    if (kind != PrefetchSchedulerAction::StorageDrain) {
        scheduler_(b, time, future);
        return;
    }
    auto fact = facts_.at(b.rank);
    fact.ts = time;
    auto & samples = action.run->storage_samples;
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
