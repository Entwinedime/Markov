#include "markov/trace_graph/modules/hicache/model/prefetch_queue_execution.hpp"

#include <algorithm>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::model {

HiCachePrefetchQueueExecution::HiCachePrefetchQueueExecution(HiCacheState & state, std::map<int, HiCacheFact> facts, PrefetchQueueTiming timing)
    : state_(state),
      facts_(std::move(facts)),
      timing_(std::move(timing)) {
    const auto & agreement = timing_.agreement;
    const bool local = facts_.size() == 1 && agreement.calls.empty();
    if (facts_.empty() || !agreement.issue.empty() || (!local && agreement.calls.size() != facts_.size()))
        throw std::invalid_argument("Prefetch queue execution requires complete MIN timing");
    for (const auto & [rank, fact] : facts_) {
        (void)timing_.cpu.at(rank);
        if (agreement.calls.empty()) continue; // Single-rank local operation.
        const auto & peers = agreement.calls.at(rank).entry_ranks;
        if (peers.size() != facts_.size()
            || std::ranges::any_of(facts_, [&](const auto & item) { return std::ranges::find(peers, item.first) == peers.end(); }))
            throw std::invalid_argument("Prefetch queue MIN must include every participant");
    }
}

CpuRankNodes HiCachePrefetchQueueExecution::start(const CpuRankNodes & entries, simulation::FutureDag & future) {
    if (!owners_.empty() || entries.size() != facts_.size()) throw std::logic_error("Prefetch queue entries must be supplied once for every rank");
    owners_ = entries;
    for (const auto & [rank, fact] : facts_) {
        entries_[rank] = boundary(rank, Step::Entry, entries.at(rank), 0, future);
        returned_[rank] = boundary(rank, Step::Finished, entries_.at(rank), 0, future);
    }
    return returned_;
}

size_t HiCachePrefetchQueueExecution::boundary(int rank, Step step, size_t previous, uint64_t delay, simulation::FutureDag & future) {
    const auto owner = owners_.at(rank);
    const auto work =
        future.append({ .name = "hicache_prefetch_queue_cpu", .category = "hicache_patch", .duration = delay }, simulation::BoundaryOrder::Ordinary, owner);
    const auto point = future.append({ .name = "hicache_prefetch_queue_boundary", .category = "hicache_patch" }, simulation::BoundaryOrder::Ordinary, owner);
    future.depend(previous, work);
    future.depend(work, point);
    events_.emplace(point, Event{ rank, step });
    return point;
}

void HiCachePrefetchQueueExecution::enter(int rank, size_t node, const HiCacheFact & fact, simulation::FutureDag & future) {
    const bool active = state_.prefetch_candidate_operation(fact) != nullptr;
    if (participating_ && *participating_ != active) throw std::logic_error("Prefetch query cannot mix absent candidates with collective participants");
    participating_ = active;
    if (!active) return;
    if (!timing_.agreement.calls.empty() && !agreement_) {
        agreement_.emplace(timing_.agreement);
        (void)agreement_->prepare(entries_, future);
    }
    const auto & cpu = timing_.cpu.at(rank);
    const auto sampled = boundary(rank, Step::Sample, node, cpu.before_sample, future);
    const auto min_entry = future.append({ .name = "hicache_prefetch_queue_to_min", .category = "hicache_patch", .duration = cpu.sample_to_min },
                                         simulation::BoundaryOrder::Ordinary,
                                         owners_.at(rank));
    future.depend(sampled, min_entry);
    const auto call = agreement_ ? agreement_->submit(rank, min_entry, future) : min_entry;
    const auto applied = boundary(rank, Step::Apply, call, cpu.min_to_apply, future);
    future.depend(applied, returned_.at(rank));
}

std::optional<int> HiCachePrefetchQueueExecution::advance(size_t node, uint64_t time, simulation::FutureDag & future) {
    const auto found = events_.find(node);
    if (found == events_.end()) return std::nullopt;
    const auto [rank, step] = found->second;
    events_.erase(found);
    auto fact = facts_.at(rank);
    fact.ts = time;
    if (step == Step::Entry) {
        enter(rank, node, fact, future);
        return std::nullopt;
    }
    if (step == Step::Finished) return rank;
    if (step == Step::Sample) {
        samples_[rank] = state_.query_prefetch_storage(fact);
        return std::nullopt;
    }
    if (samples_.size() != facts_.size()) throw std::logic_error("Prefetch queue MIN returned before all local samples");
    auto common = samples_.begin()->second;
    for (const auto & [peer, sample] : samples_) common = std::min(common, sample);
    state_.complete_prefetch_query(fact, common);
    return std::nullopt;
}

} // namespace markov::trace_graph::modules::hicache::model
