#include "markov/trace_graph/source_dag/prefetch.hpp"
#include "markov/trace_graph/core/numeric.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace markov::trace_graph::source_dag {
using namespace modules::hicache;
using namespace modules::hicache::model;

namespace {
using Control = PrefetchControlObservation;
using Fact = patch::HiCacheSourceFactNode;
uint64_t end(const core::TraceEvent & e) { return e.ts + e.dur; }
bool complete(const patch::HiCacheTimingIntervalOwnership & interval) { return interval.status == "ready" || interval.status == "zero_duration"; }
} // namespace

PrefetchSchedulerBody observe_prefetch_scheduler_body(const patch::HiCacheSourceDagIndex & source, const CpuCollectiveObservation & collectives,
                                                      const PrefetchWorkerObservations & workers, const std::vector<Control> & previous,
                                                      const std::vector<Control> & next) {
    PrefetchSchedulerBody body;
    const auto fail = [&](std::string issue) {
        body.issue = std::move(issue);
        return body;
    };
    if (previous.empty() || previous.size() != next.size()) return fail("scheduler_body_missing_ranks");
    std::map<int, const Control *> following;
    for (const auto & c : next) {
        if (!c.issue.empty() || !c.rank || c.cpu.empty() || !following.emplace(*c.rank, &c).second) return fail("scheduler_body_invalid_next_progress");
    }
    const auto & events = source.graph().runtime_observations();
    std::map<int, const Fact *> facts;
    std::vector<size_t> rounds;
    std::set<size_t> included_events;
    for (const auto & c : previous) {
        if (!c.issue.empty() || !c.rank || c.cpu.empty() || !following.contains(*c.rank) || facts.contains(*c.rank))
            return fail("scheduler_body_invalid_previous_progress");
        const auto * f = source.fact_node(c.progress_fact);
        const auto & after = *following.at(*c.rank);
        const auto * n = source.fact_node(after.progress_fact);
        if (!f || !n || f->progress_ready != false || f->pid != n->pid || f->tid != n->tid || f->request_id != n->request_id)
            return fail("scheduler_body_progress_identity_or_decision");
        const auto begin = c.cpu.back().cpu.interval_end_us, finish = after.cpu.front().cpu.interval_start_us;
        if (finish < begin) return fail("scheduler_body_overlapping_progress");
        auto interval = source.timing_interval_ownership(f->pid, f->tid, begin, finish - begin);
        if (!complete(interval)) return fail("scheduler_body_incomplete_cpu");
        body.lanes[*c.rank] = interval;
        facts[*c.rank] = f;
        std::vector<std::pair<uint64_t, size_t>> sequence;
        for (size_t r = 0; r < collectives.rounds.size(); ++r)
            for (const auto & call : collectives.rounds[r].calls) {
                const auto & e = events.at(call.observation);
                if (call.rank == *c.rank && e.pid == f->pid && e.tid == f->tid && e.ts >= begin && end(e) <= finish) sequence.emplace_back(e.ts, r);
            }
        std::ranges::sort(sequence);
        std::vector<size_t> ids;
        for (const auto & [at, r] : sequence) ids.push_back(r);
        if (facts.size() == 1) rounds = ids;
        else if (rounds != ids) return fail("scheduler_body_collective_order_or_membership");
        for (const auto & other : source.fact_nodes())
            if (other.pid == f->pid && other.tid == f->tid && other.node_id != f->node_id && other.node_id != n->node_id && other.timestamp_us < finish
                && other.timestamp_us + other.duration_us > begin)
                return fail("scheduler_body_contains_other_cache_work");
    }
    if (rounds.empty()) return fail("scheduler_body_no_observed_communication");
    CpuRankTimes cursor;
    for (const auto & [rank, interval] : body.lanes) cursor[rank] = interval.interval_start_us;
    for (const auto r : rounds) {
        const auto & round = collectives.rounds.at(r);
        if (round.calls.size() != facts.size()) return fail("scheduler_body_partial_group");
        const auto & first = events.at(round.calls.front().observation);
        const auto role = first.arg("role");
        PrefetchSchedulerAction action;
        if (role == "request_receive") {
            action = PrefetchSchedulerAction::Receive;
            if (first.arg("operation") != "broadcast" || first.arg("dtype") != "torch.int64" || first.arg_u64("numel") != 1)
                return fail("scheduler_body_receive_contains_payload");
        }
        else {
            if (role == "write_completion_check") action = PrefetchSchedulerAction::WriteCompletion;
            else if (role == "load_completion_check") action = PrefetchSchedulerAction::LoadCompletion;
            else if (role == "storage_control_drain") action = PrefetchSchedulerAction::StorageDrain;
            else return fail("scheduler_body_unknown_collective_role");
            const auto reduction = first.arg("reduce_op");
            if (first.arg("operation") != "all_reduce" || first.arg("dtype") != "torch.int32" || (reduction != "MIN" && reduction != "RedOpType.MIN")
                || first.arg_u64("numel") != (action == PrefetchSchedulerAction::StorageDrain ? 3 : 1))
                return fail("scheduler_body_invalid_min");
        }
        PrefetchSchedulerStep step{ .action = action, .source_round = r, .communication = observe_cpu_collective_timing(source.graph(), round) };
        if (!step.communication.issue.empty()) return fail("scheduler_body_communication:" + step.communication.issue);
        for (const auto & call : round.calls) {
            const auto & e = events.at(call.observation);
            const auto * f = facts.at(call.rank);
            if (e.ts < cursor.at(call.rank)) return fail("scheduler_body_overlapping_calls");
            step.before[call.rank] = source.timing_interval_ownership(f->pid, f->tid, cursor.at(call.rank), e.ts - cursor.at(call.rank));
            auto applied = end(e);
            included_events.insert(call.observation);
            if (action == PrefetchSchedulerAction::StorageDrain) {
                const StorageDrainObservation * drain = nullptr;
                for (const auto & d : workers.drains)
                    if (events.at(d.observation).pid == f->pid && events.at(d.observation).tid == f->tid
                        && std::ranges::find(d.agreement_rounds, r) != d.agreement_rounds.end()) {
                        if (drain) return fail("scheduler_body_nonunique_drain");
                        drain = &d;
                    }
                if (!drain || !drain->issue.empty() || drain->agreement_rounds.size() != 1) return fail("scheduler_body_missing_drain");
                const auto & d = events.at(drain->observation);
                if (d.ts < end(e) || end(d) > body.lanes.at(call.rank).interval_end_us) return fail("scheduler_body_drain_outside_call");
                applied = end(d);
                included_events.insert(drain->observation);
            }
            else if (action == PrefetchSchedulerAction::WriteCompletion || action == PrefetchSchedulerAction::LoadCompletion) {
                const auto name = action == PrefetchSchedulerAction::LoadCompletion ? "runtime.hicache.load_completion" : "runtime.hicache.write_completion";
                for (size_t i = 0; i < events.size(); ++i) {
                    const auto & check = events[i];
                    if (check.name != name || check.pid != f->pid || check.tid != f->tid || check.ts > e.ts || end(check) < end(e)) continue;
                    if (!step.completion_observations.emplace(call.rank, i).second || included_events.contains(i))
                        return fail("scheduler_body_nonunique_completion");
                    if (check.arg("status") != "returned" || check.arg("blocking") != "false" || check.ts < cursor.at(call.rank)
                        || end(check) > body.lanes.at(call.rank).interval_end_us)
                        return fail("scheduler_body_invalid_completion");
                    applied = end(check);
                    included_events.insert(i);
                }
            }
            step.after[call.rank] = source.timing_interval_ownership(f->pid, f->tid, end(e), applied - end(e));
            if (!complete(step.before.at(call.rank)) || !complete(step.after.at(call.rank))) return fail("scheduler_body_incomplete_local_work");
            cursor[call.rank] = applied;
        }
        body.steps.push_back(std::move(step));
    }
    for (const auto & [rank, f] : facts) {
        const auto & lane = body.lanes.at(rank);
        if (cursor.at(rank) > lane.interval_end_us) return fail("scheduler_body_end_before_last_return");
        body.tail[rank] = source.timing_interval_ownership(f->pid, f->tid, cursor.at(rank), lane.interval_end_us - cursor.at(rank));
        if (!complete(body.tail.at(rank))) return fail("scheduler_body_incomplete_tail");
        for (size_t i = 0; i < events.size(); ++i) {
            const auto & e = events[i];
            if (e.pid == f->pid && e.tid == f->tid && e.ts < lane.interval_end_us && end(e) > lane.interval_start_us && !included_events.contains(i))
                return fail("scheduler_body_unhandled_runtime_work:" + e.name);
        }
    }
    return body;
}

PrefetchSchedulerTiming prefetch_scheduler_timing(const PrefetchSchedulerBody & observed, const core::DagGraph & graph) {
    if (!observed.issue.empty() || observed.steps.empty() || observed.lanes.empty())
        throw std::invalid_argument("Scheduler timing requires a fully observed loop body");
    PrefetchSchedulerTiming timing;
    const auto local = [&](const patch::HiCacheTimingIntervalOwnership & interval) {
        if (interval.owned_node_duration_us > interval.observed_duration_us
            || interval.owned_gap_duration_us != interval.observed_duration_us - interval.owned_node_duration_us)
            throw std::invalid_argument("Scheduler local work and gap do not account for the observed interval");
        PrefetchSchedulerLocalTiming service;
        for (const auto id : interval.owned_node_ids) {
            const auto & event = graph.event_for_node(id);
            service.cpu_us += graph.cpu_service_cost().duration({ event.pid, event.tid }, event.ts, event.ts + event.dur);
        }
        for (const auto & gap : interval.owned_gap_slices) {
            const auto & event = graph.event_for_node(gap.owner_node_id);
            service.gap_us += graph.cpu_service_cost().duration({ event.pid, event.tid }, gap.owned_start_us, gap.owned_end_us);
        }
        return service;
    };
    for (const auto & [rank, lane] : observed.lanes) timing.tail[rank] = local(observed.tail.at(rank));
    for (const auto & step : observed.steps) {
        PrefetchSchedulerStepTiming measured{ .communication = step.communication };
        for (const auto & [rank, lane] : observed.lanes) {
            measured.before[rank] = local(step.before.at(rank));
            measured.after[rank] = local(step.after.at(rank));
        }
        if (!timing.steps.emplace(step.action, std::move(measured)).second)
            throw std::invalid_argument("Scheduler measurement repeats an action within one retry body");
    }
    return timing;
}

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

std::optional<PrefetchSchedulerTiming> observe_prefetch_stop_timing(const patch::HiCacheSourceDagIndex & source, const CpuCollectiveObservation & rounds,
                                                                    const std::map<int, std::vector<PrefetchControlObservation>> & ranks) {
    if (ranks.empty()) return std::nullopt;
    std::optional<size_t> completion;
    for (const auto & [rank, calls] : ranks) {
        if (calls.size() != 1 || calls.front().local_return || !calls.front().stop || calls.front().state_round) return std::nullopt;
        const auto & call = calls.front();
        if (!call.issue.empty() || call.rank != rank || !call.completion_round || call.cpu.empty())
            throw std::invalid_argument("Immediate stop requires complete ranked progress observations");
        if (completion && completion != call.completion_round) throw std::invalid_argument("Immediate stop ranks do not share a completion MIN");
        completion = call.completion_round;
    }
    const auto measured = observe_cpu_collective_timing(source.graph(), rounds.rounds.at(*completion));
    if (!measured.issue.empty() || measured.calls.size() != ranks.size()
        || std::ranges::any_of(measured.calls, [&](const auto & item) { return !ranks.contains(item.first); }))
        throw std::invalid_argument("Immediate stop requires a complete all-rank MIN");
    return PrefetchSchedulerTiming{};
}

std::optional<PrefetchSchedulerTiming> observe_prefetch_wait_timing(const patch::HiCacheSourceDagIndex & source, const CpuCollectiveObservation & rounds,
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
    std::vector<PrefetchControlObservation> first, next;
    for (const auto & [rank, calls] : ranks) {
        if (!calls.back().completion_round) throw std::invalid_argument("Active wait has no terminal completion round");
        first.push_back(calls.front());
        next.push_back(calls[1]);
    }
    const auto body = observe_prefetch_scheduler_body(source, rounds, workers, first, next);
    if (!body.issue.empty()) throw std::runtime_error("Active wait scheduler measurement incomplete: " + body.issue);
    for (const auto & step : body.steps)
        if ((step.action == PrefetchSchedulerAction::WriteCompletion || step.action == PrefetchSchedulerAction::LoadCompletion)
            && step.completion_observations.size() != ranks.size())
            throw std::runtime_error("Wait template has no complete confirmation boundary");
    return prefetch_scheduler_timing(body, source.graph());
}

} // namespace markov::trace_graph::source_dag
