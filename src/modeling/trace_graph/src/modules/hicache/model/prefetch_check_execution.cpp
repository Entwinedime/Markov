#include "markov/trace_graph/modules/hicache/model/prefetch_check_execution.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/modules/hicache/missing_cost.hpp"

#include <algorithm>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::model {
namespace {
uint64_t cost(const std::optional<uint64_t> & observed) {
    if (!observed) throw std::invalid_argument("Prefetch branch lacks observed CPU cost");
    return *observed;
}
} // namespace

PrefetchCheckCpuTiming observe_prefetch_check_cpu(const PrefetchControlObservation & observed, const patch::HiCacheSourceDagIndex * source) {
    if (!observed.issue.empty()) throw std::invalid_argument("Prefetch CPU template requires a complete observed call");
    const auto * fact = source ? source->fact_node(observed.progress_fact) : nullptr;
    if (source && !fact) throw std::invalid_argument("Prefetch service has no source identity");
    const auto duration = [&](std::initializer_list<std::string_view> roles) {
        uint64_t total = 0;
        for (const auto role : roles) {
            const auto found = std::ranges::find(observed.cpu, role, &PrefetchControlCpuInterval::role);
            if (found == observed.cpu.end()) throw std::invalid_argument("Missing prefetch CPU interval: " + std::string(role));
            const auto service =
                source ? source->graph().cpu_service_cost().duration({ fact->pid, fact->tid }, found->cpu.interval_start_us, found->cpu.interval_end_us)
                       : found->cpu.observed_duration_us;
            total = core::checked_add_u64(total, service, "Prefetch branch service overflow");
        }
        return total;
    };
    PrefetchCheckCpuTiming cpu;
    if (observed.local_return) {
        cpu.no_operation_return = duration({ "local_return" });
        return cpu;
    }
    cpu.entry_to_check = duration({ "before_check" });
    if (observed.state_round) {
        cpu.check_to_max = duration({ "check_before_max" });
        (observed.stop ? cpu.max_to_stop : cpu.max_to_false_return) = duration({ "check_after_max", "after_check" });
    }
    else cpu.best_effort_to_stop = duration({ "check", "after_check" });
    if (observed.stop) {
        cpu.stop_to_min = duration({ "stop_call", "before_min" });
        cpu.min_to_visible = duration({ "after_min" });
    }
    return cpu;
}

HiCachePrefetchCheckExecution::HiCachePrefetchCheckExecution(HiCacheState & state, std::map<int, HiCacheFact> facts, PrefetchCheckTiming timing,
                                                             std::string policy)
    : state_(state),
      facts_(std::move(facts)),
      timing_(std::move(timing)),
      immediate_(policy == "best_effort") {
    if ((!immediate_ && policy != "wait_complete" && policy != "timeout") || facts_.empty())
        throw std::invalid_argument("Invalid prefetch check policy or ranks");
    for (const auto & [rank, fact] : facts_) (void)timing_.cpu.at(rank);
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
    const auto & cpu = timing_.cpu.at(rank);
    if (!active) {
        if (!cpu.no_operation_return)
            throw MissingCostEvidence("execution_control/prefetch",
                                      {
                                          { "program", "local_return" }
            },
                                      "Prefetch early return requires observed local CPU cost");
        const auto tail = future.append({ .name = "hicache_prefetch_no_operation_cpu", .duration = *cpu.no_operation_return },
                                        simulation::BoundaryOrder::Ordinary,
                                        owners_.at(rank));
        future.depend(tail, returned_.at(rank));
        return;
    }
    if (!first_collective_) {
        if (timing_.completion_min.calls.empty())
            throw MissingCostEvidence("execution_control/prefetch",
                                      {
                                          { "program", immediate_ ? "best_effort" : "wait_complete" }
            },
                                      "Active prefetch requires base or shared stop/wait timing");
        for (const auto * collective : { &timing_.state_max, &timing_.completion_min }) {
            if (immediate_ && collective == &timing_.state_max) continue;
            if (!collective->issue.empty() || collective->calls.size() != facts_.size())
                throw std::invalid_argument("Prefetch check requires complete collective timing");
            for (const auto & [participant, original] : facts_) {
                const auto & peers = collective->calls.at(participant).entry_ranks;
                if (peers.size() != facts_.size()
                    || std::ranges::any_of(facts_, [&](const auto & item) { return std::ranges::find(peers, item.first) == peers.end(); }))
                    throw std::invalid_argument("Prefetch check requires an all-rank MAX/MIN group");
            }
        }
        first_collective_.emplace(immediate_ ? timing_.completion_min : timing_.state_max);
        (void)first_collective_->prepare(entries_, future);
    }
    const auto sample = boundary(rank, Step::Sample, node, cpu.entry_to_check, future);
    size_t call_entry;
    if (immediate_) {
        allowed_ = true;
        call_entry = stop_entry(rank, sample, true, future);
    }
    else {
        call_entry = future.append({ .name = "hicache_prefetch_check_to_max", .duration = cost(cpu.check_to_max) },
                                   simulation::BoundaryOrder::Ordinary,
                                   owners_.at(rank));
        future.depend(sample, call_entry);
    }
    const auto call = first_collective_->submit(rank, call_entry, future);
    const auto after = boundary(rank, immediate_ ? Step::Visible : Step::MaxReturned, call, immediate_ ? cost(cpu.min_to_visible) : 0, future);
    if (!immediate_) max_returned_[rank] = after;
    future.depend(after, returned_.at(rank));
}

size_t HiCachePrefetchCheckExecution::stop_entry(int rank, size_t predecessor, bool immediate, simulation::FutureDag & future) {
    const auto & cpu = timing_.cpu.at(rank);
    const auto stop = boundary(rank, Step::Stop, predecessor, cost(immediate ? cpu.best_effort_to_stop : cpu.max_to_stop), future);
    const auto entry =
        future.append({ .name = "hicache_prefetch_stop_to_min", .duration = cost(cpu.stop_to_min) }, simulation::BoundaryOrder::Ordinary, owners_.at(rank));
    future.depend(stop, entry);
    return entry;
}

void HiCachePrefetchCheckExecution::terminal(const CpuRankNodes & predecessors, bool immediate, simulation::FutureDag & future) {
    CpuRankNodes min_entries;
    for (const auto & [rank, node] : predecessors) min_entries[rank] = stop_entry(rank, node, immediate, future);
    const auto calls = append_cpu_collective(timing_.completion_min, min_entries, future);
    for (const auto & [rank, call] : calls) {
        const auto visible = boundary(rank, Step::Visible, call, cost(timing_.cpu.at(rank).min_to_visible), future);
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
                const auto tail =
                    future.append({ .name = "hicache_prefetch_false_return_cpu", .duration = cost(timing_.cpu.at(participant).max_to_false_return) },
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
