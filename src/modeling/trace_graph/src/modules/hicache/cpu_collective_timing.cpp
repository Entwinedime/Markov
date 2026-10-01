#include "markov/trace_graph/modules/hicache/cpu_collective_timing.hpp"
#include <algorithm>
#include <bit>
#include <set>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache {
CpuCollectiveTiming observe_cpu_collective_timing(const core::DagGraph & graph, const CpuCollectiveRound & round) {
    CpuCollectiveTiming result;
    if (!round.issue.empty() || round.calls.empty() || std::ranges::any_of(round.calls, [](const auto & c) { return !c.issue.empty(); })) {
        result.issue = "incomplete_round";
        return result;
    }
    const auto & first = graph.runtime_observations()[round.calls.front().observation];
    const auto operation = first.arg("operation");
    const auto size = round.members.size();
    size_t root = 0;
    if (operation == "broadcast") {
        root = size;
        if (!first.arg("group_src").empty()) root = std::stoul(first.arg("group_src"));
        else if (!first.arg("src").empty()) root = std::ranges::find(round.members, std::stoi(first.arg("src"))) - round.members.begin();
        if (root >= size) {
            result.issue = "broadcast_root_not_in_group";
            return result;
        }
    }
    else if (operation != "all_reduce") {
        result.issue = "unsupported_collective_operation";
        return result;
    }
    CpuRankTimes entries;
    for (const auto & call : round.calls) entries[call.rank] = graph.event_for_node(*call.worker).ts;
    for (const auto & call : round.calls) {
        auto & timing = result.calls[call.rank];
        if (operation == "all_reduce") timing.entry_ranks = round.members;
        else {
            const auto member = std::ranges::find(round.members, call.rank) - round.members.begin();
            const size_t rank = (member + size - root) % size;
            const auto actual = [&](size_t v) { return round.members[(v + root) % size]; };
            timing.entry_ranks.push_back(call.rank);
            // Gloo binomial tree: receive before forwarding, TCP send waits
            // for direct-child entry. These are not parent-return edges.
            for (size_t ancestor = rank; ancestor != 0;) {
                ancestor ^= std::bit_floor(ancestor);
                timing.entry_ranks.push_back(actual(ancestor));
            }
            for (size_t bit = 1; bit < size; bit <<= 1)
                if (rank < bit && rank + bit < size) timing.entry_ranks.push_back(actual(rank + bit));
        }
        uint64_t latest = 0;
        for (const auto rank : timing.entry_ranks) latest = std::max(latest, entries.at(rank));
        const auto & event = graph.runtime_observations()[call.observation];
        const auto & submit = graph.event_for_node(*call.submission);
        const auto & worker = graph.event_for_node(*call.worker);
        const auto submitted = submit.ts + submit.dur, worked = worker.ts + worker.dur, returned = event.ts + event.dur;
        if (submit.ts < event.ts || worker.ts < submit.ts || worked < latest || returned < std::max(submitted, worked)) {
            result.issue = "inconsistent_call_timing";
            return result;
        }
        // Generated nodes have no observed CPU coordinates: transfer the
        // already measured local service, not the uncorrected trace duration.
        // Dispatch and peer rendezvous remain separate wall-clock evidence;
        // caller CPU correction must not shorten another thread's waiting.
        const core::CpuServiceCost::Lane caller{ event.pid, event.tid };
        timing.before_submit = graph.cpu_service_cost().duration(caller, event.ts, submit.ts);
        timing.submission = graph.cpu_service_cost().duration(caller, submit.ts, submitted);
        timing.dispatch = worker.ts - submit.ts;
        timing.worker_remainder = worked - latest;
        timing.after_join = graph.cpu_service_cost().duration(caller, std::max(submitted, worked), returned);
    }
    return result;
}

CpuCollectiveTiming observe_cpu_all_reduce_envelope(const core::DagGraph & graph, const CpuCollectiveRound & round) {
    CpuCollectiveTiming timing;
    const std::set<int> expected(round.members.begin(), round.members.end());
    std::set<int> seen;
    uint64_t latest = 0;
    if (!round.issue.empty() || expected.empty() || expected.size() != round.members.size() || round.calls.size() != expected.size()) {
        timing.issue = "incomplete_envelope_round";
        return timing;
    }
    const auto & first = graph.runtime_observations().at(round.calls.front().observation);
    for (const auto & call : round.calls) {
        const auto & e = graph.runtime_observations().at(call.observation);
        if (!seen.insert(call.rank).second || !expected.contains(call.rank) || e.arg("operation") != "all_reduce" || e.arg("async_op") != "false"
            || e.arg("status") != "returned" || e.arg_u64("sequence_after") <= e.arg_u64("sequence_before")) {
            timing.issue = "not_complete_synchronous_all_reduce";
            return timing;
        }
        for (const auto key : { "operation", "numel", "dtype", "reduce_op", "role", "request_id" })
            if (e.arg(key) != first.arg(key)) {
                timing.issue = "envelope_signature_mismatch";
                return timing;
            }
        latest = std::max(latest, e.ts);
    }
    for (const auto & call : round.calls) {
        const auto & e = graph.runtime_observations().at(call.observation);
        if (e.ts + e.dur < e.ts || e.ts + e.dur < latest) {
            timing.calls.clear();
            timing.issue = "envelope_return_before_peer_entry";
            return timing;
        }
        timing.calls[call.rank] = { .entry_ranks = round.members, .envelope_remainder_us = e.ts + e.dur - latest };
    }
    return timing;
}

namespace {
bool envelope_timing(const CpuCollectiveTiming & timing) {
    const bool envelope = !timing.calls.empty() && timing.calls.begin()->second.envelope_remainder_us.has_value();
    if (std::ranges::any_of(timing.calls, [&](const auto & item) { return item.second.envelope_remainder_us.has_value() != envelope; }))
        throw std::invalid_argument("Collective cannot mix call-entry and worker-entry timing evidence");
    return envelope;
}
} // namespace


CpuCollectiveExecution::CpuCollectiveExecution(CpuCollectiveTiming timing) : timing_(std::move(timing)) {
    if (!timing_.issue.empty() || timing_.calls.empty()) throw std::invalid_argument("Collective execution requires observed call timing");
    (void)envelope_timing(timing_);
}

CpuRankNodes CpuCollectiveExecution::prepare(const CpuRankNodes & entries, simulation::FutureDag & future) {
    if (!owners_.empty() || entries.size() != timing_.calls.size()) throw std::logic_error("Collective gates require all entries exactly once");
    owners_ = entries;
    for (const auto & [rank, call] : timing_.calls) {
        const auto owner = owners_.at(rank);
        arrivals_[rank] = future.append({ .name = "collective entry gate", .category = "execution_gate" }, simulation::BoundaryOrder::Ordinary, owner);
        returned_[rank] = future.append(
            {
                .name = "collective return gate",
                .category = "execution_gate",
                .attrs = call.envelope_remainder_us
                             ? std::unordered_map<std::string, std::string>{ { "timing_evidence", "call_envelope" }, { "includes_residual_cpu", "true" } }
                             : std::unordered_map<std::string, std::string>{}
        },
            simulation::BoundaryOrder::Ordinary,
            owner);
        future.depend(owner, arrivals_.at(rank));
        future.depend(owner, returned_.at(rank));
    }
    return returned_;
}

size_t CpuCollectiveExecution::submit(int rank, size_t entry, simulation::FutureDag & future) {
    const auto & call = timing_.calls.at(rank);
    const auto owner = owners_.at(rank);
    if (!submitted_.insert(rank).second) throw std::logic_error("Collective participants cannot be submitted twice");
    const auto task = [&](const char * name, uint64_t duration, std::initializer_list<size_t> before) {
        const auto id = future.append({ .name = name, .category = "cpu_collective", .duration = duration }, simulation::BoundaryOrder::Ordinary, owner);
        for (const auto prior : before) future.depend(prior, id);
        return id;
    };
    size_t work;
    if (call.envelope_remainder_us) {
        future.depend(entry, arrivals_.at(rank));
        work = task("collective call-envelope remainder", *call.envelope_remainder_us, {});
        future.depend(work, returned_.at(rank));
    }
    else {
        const auto before = task("collective before submit", call.before_submit, { entry });
        const auto submitted = task("collective submission", call.submission, { before });
        const auto dispatch = task("collective dispatch", call.dispatch, { before });
        future.depend(dispatch, arrivals_.at(rank));
        work = task("collective worker remainder", call.worker_remainder, { arrivals_.at(rank) });
        const auto returned = task("collective return", call.after_join, { submitted, work });
        future.depend(returned, returned_.at(rank));
    }
    for (const auto peer : call.entry_ranks) future.depend(arrivals_.at(peer), work);
    return returned_.at(rank);
}

CpuRankNodes append_cpu_collective(const CpuCollectiveTiming & timing, const CpuRankNodes & entries, simulation::FutureDag & future) {
    CpuCollectiveExecution execution(timing);
    const auto returned = execution.prepare(entries, future);
    for (const auto & [rank, entry] : entries) (void)execution.submit(rank, entry, future);
    return returned;
}

} // namespace markov::trace_graph::modules::hicache
