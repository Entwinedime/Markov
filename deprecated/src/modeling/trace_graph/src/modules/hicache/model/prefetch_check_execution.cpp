#include "markov/trace_graph/modules/hicache/model/prefetch_check_execution.hpp"
#include "markov/trace_graph/structure/cpu_collective.hpp"

#include <algorithm>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::model {
HiCachePrefetchCheckExecution::HiCachePrefetchCheckExecution(HiCacheState & state, std::map<int, HiCacheFact> facts, std::string policy,
                                                             PrefetchCheckLocalCost local_cost, PrefetchCheckCollectiveCost collective_cost)
    : state_(state),
      facts_(std::move(facts)),
      local_cost_(std::move(local_cost)),
      collective_cost_(std::move(collective_cost)),
      immediate_(policy == "best_effort") {
    if ((!immediate_ && policy != "wait_complete" && policy != "timeout") || facts_.empty())
        throw std::invalid_argument("Invalid prefetch check policy or ranks");
}

CpuCollectiveTiming HiCachePrefetchCheckExecution::collective(bool completion) const {
    std::vector<int> participants;
    for (const auto & [rank, fact] : facts_) participants.push_back(rank);
    const auto dependencies = structure::cpu_collective_entries(structure::CpuCollectiveKind::AllReduce, participants);

    CpuCollectiveTiming timing;
    for (const auto rank : participants) {
        auto call = collective_cost_(rank, completion);
        call.entry_ranks = dependencies.at(rank);
        timing.calls.emplace(rank, std::move(call));
    }
    return timing;
}

size_t HiCachePrefetchCheckExecution::boundary(int rank, Step step, size_t predecessor, uint64_t delay, simulation::FutureDag & future) {
    const auto owner = owners_.at(rank);
    const auto work =
        future.append({ .name = "hicache_prefetch_control_cpu", .category = "hicache_patch", .duration = delay }, simulation::BoundaryOrder::Ordinary, owner);
    const auto point = future.append({ .name = "hicache_prefetch_control_boundary", .category = "hicache_patch" }, simulation::BoundaryOrder::Ordinary, owner);
    future.depend(predecessor, work);
    future.depend(work, point);
    events_.emplace(point, Event{ rank, step });
    return point;
}

CpuRankNodes HiCachePrefetchCheckExecution::start(const CpuRankNodes & entries, simulation::FutureDag & future) {
    if (!owners_.empty() || entries.size() != facts_.size()) throw std::invalid_argument("Prefetch check entries must be supplied once for every rank");
    owners_ = entries;
    for (const auto & [rank, fact] : facts_) {
        entries_[rank] = boundary(rank, Step::Entry, entries.at(rank), 0, future);
        returned_[rank] = boundary(rank, Step::Finished, entries_.at(rank), 0, future);
    }
    return returned_;
}

void HiCachePrefetchCheckExecution::enter(int rank, size_t node, const HiCacheFact & fact, simulation::FutureDag & future) {
    const bool active = state_.sample_prefetch_check(fact).ongoing;
    if (participating_ && *participating_ != active) throw std::logic_error("Prefetch progress cannot mix local early returns with collective participants");
    participating_ = active;
    if (!active) {
        const auto tail = future.append({ .name = "hicache_prefetch_no_operation_cpu", .duration = local_cost_(rank, "no_operation_return") },
                                        simulation::BoundaryOrder::Ordinary,
                                        owners_.at(rank));
        future.depend(tail, returned_.at(rank));
        return;
    }
    if (!first_collective_) {
        first_collective_.emplace(collective(immediate_));
        (void)first_collective_->prepare(entries_, future);
    }
    const auto sample = boundary(rank, Step::Sample, node, local_cost_(rank, "entry_to_check"), future);
    size_t call_entry;
    if (immediate_) {
        allowed_ = true;
        call_entry = stop_entry(rank, sample, true, future);
    }
    else {
        call_entry = future.append({ .name = "hicache_prefetch_check_to_max", .duration = local_cost_(rank, "check_to_max") },
                                   simulation::BoundaryOrder::Ordinary,
                                   owners_.at(rank));
        future.depend(sample, call_entry);
    }
    const auto call = first_collective_->submit(rank, call_entry, future);
    const auto after = boundary(rank, immediate_ ? Step::Visible : Step::MaxReturned, call, immediate_ ? local_cost_(rank, "min_to_visible") : 0, future);
    if (!immediate_) max_returned_[rank] = after;
    future.depend(after, returned_.at(rank));
}

size_t HiCachePrefetchCheckExecution::stop_entry(int rank, size_t predecessor, bool immediate, simulation::FutureDag & future) {
    const auto stop = boundary(rank, Step::Stop, predecessor, local_cost_(rank, immediate ? "best_effort_to_stop" : "max_to_stop"), future);
    const auto entry = future.append({ .name = "hicache_prefetch_stop_to_min", .duration = local_cost_(rank, "stop_to_min") },
                                     simulation::BoundaryOrder::Ordinary,
                                     owners_.at(rank));
    future.depend(stop, entry);
    return entry;
}

void HiCachePrefetchCheckExecution::terminal(const CpuRankNodes & predecessors, bool immediate, simulation::FutureDag & future) {
    CpuRankNodes min_entries;
    for (const auto & [rank, node] : predecessors) min_entries[rank] = stop_entry(rank, node, immediate, future);
    const auto calls = append_cpu_collective(collective(true), min_entries, future);
    for (const auto & [rank, call] : calls) {
        const auto visible = boundary(rank, Step::Visible, call, local_cost_(rank, "min_to_visible"), future);
        future.depend(visible, returned_.at(rank));
    }
}

std::optional<bool> HiCachePrefetchCheckExecution::advance(size_t node, uint64_t time, simulation::FutureDag & future) {
    const auto found = events_.find(node);
    if (found == events_.end()) return std::nullopt;
    const auto [rank, step] = found->second;
    events_.erase(found);
    auto fact = facts_.at(rank);
    fact.ts = time;
    if (step == Step::Entry) enter(rank, node, fact, future);
    else if (step == Step::Sample) {
        samples_[rank] = state_.sample_prefetch_check(fact);
        if (!samples_.at(rank).ongoing) throw std::logic_error("Active-prefetch check received an absent or already settled operation");
    }
    else if (step == Step::MaxReturned && !allowed_) {
        if (samples_.size() != facts_.size()) throw std::logic_error("Prefetch MAX returned before all samples");
        const bool all = std::ranges::all_of(samples_, [](const auto & item) { return item.second.can_stop; });
        const bool terminated = std::ranges::any_of(samples_, [](const auto & item) { return item.second.terminated; });
        allowed_ = all || terminated;
        if (*allowed_) terminal(max_returned_, false, future);
        else
            for (const auto & [participant, predecessor] : max_returned_) {
                const auto tail = future.append({ .name = "hicache_prefetch_false_return_cpu", .duration = local_cost_(participant, "max_to_false_return") },
                                                simulation::BoundaryOrder::Ordinary,
                                                owners_.at(participant));
                future.depend(predecessor, tail);
                future.depend(tail, returned_.at(participant));
            }
    }
    else if (step == Step::Stop) { stopped_[rank] = state_.stop_prefetch(fact); }
    else if (step == Step::Visible) {
        if (stopped_.size() != facts_.size()) throw std::logic_error("Prefetch MIN returned before all stop snapshots");
        const auto visible = std::ranges::min_element(stopped_, {}, [](const auto & item) { return item.second; })->second;
        state_.publish_prefetch(fact, visible);
    }
    else if (step == Step::Finished) return !participating_.value() || allowed_.value();
    return std::nullopt;
}

} // namespace markov::trace_graph::modules::hicache::model
