#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/modules/hicache/prefetch_control.hpp"
#include <algorithm>
#include <set>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache {
namespace {
using Fact = patch::HiCacheSourceFactNode;
using Event = core::TraceEvent;
uint64_t end(const Event & event) { return event.ts + event.dur; }
struct ProcessEvidence {
    bool check_probe = false;
    std::set<int> ranks;
};

PrefetchControlObservation observe_call(const patch::HiCacheSourceDagIndex & source, const CpuCollectiveObservation & collectives, const Fact & progress,
                                        std::string_view policy, const ProcessEvidence & process) {
    PrefetchControlObservation result{ .progress_fact = progress.node_id };
    const auto & observations = source.graph().runtime_observations();
    const auto finish = progress.timestamp_us + progress.duration_us;
    // The semantic fact excludes probe extraction/emission around the function.
    // Keep that work in the measured call; it is not pure framework CPU service.
    const auto control = source.enclosing_control_interval_ownership(progress, "hicache.control.prefetch_progress");
    result.uses_control_envelope = control && control->status == "ready";
    const auto cpu_begin = result.uses_control_envelope ? control->interval_start_us : progress.timestamp_us;
    const auto cpu_end = result.uses_control_envelope ? control->interval_end_us : finish;
    const auto contained = [&](const Event & event, uint64_t begin, uint64_t limit) {
        return event.pid == progress.pid && event.tid == progress.tid && event.ts >= begin && end(event) <= limit;
    };
    const auto fail = [&](const char * issue) {
        result.issue = issue;
        return result;
    };
    for (size_t id = 0; id < observations.size(); ++id) {
        const auto & event = observations[id];
        if (!contained(event, progress.timestamp_us, finish)) continue;
        if (event.name != "runtime.hicache.prefetch_check" && event.name != "runtime.hicache.prefetch_stop") continue;
        if (event.arg("request_id") != progress.request_id || event.arg("status") != "returned") return fail("prefetch_boundary_identity_or_status");
        auto & slot = event.name == "runtime.hicache.prefetch_check" ? result.check : result.stop;
        if (slot) return fail("nonunique_prefetch_boundary");
        slot = id;
    }
    const Event * entry = nullptr;
    for (const auto & event : observations) {
        if (event.name != "runtime.hicache.prefetch_progress" || !contained(event, cpu_begin, cpu_end)) continue;
        if (entry) return fail("nonunique_prefetch_progress_boundary");
        if (event.arg("request_id") != progress.request_id || event.arg("status") != "returned" || !progress.progress_ready
            || event.arg("progress_ready") != (*progress.progress_ready ? "true" : "false"))
            return fail("prefetch_progress_boundary_identity_or_status");
        entry = &event;
    }
    const bool explicit_local = entry && (entry->arg("entry_branch") == "no_operation" || entry->arg("entry_branch") == "host_not_allocated");
    if (explicit_local && (result.check || result.stop)) return fail("prefetch_entry_branch_conflict");
    if (!result.check) {
        if (entry && !explicit_local) return fail("unproven_prefetch_local_return");
        if (!explicit_local && !process.check_probe) return fail("missing_prefetch_check");
        if (progress.progress_ready != true || result.stop || !result.uses_control_envelope) return fail("unproven_prefetch_local_return");
        for (const auto & event : observations)
            if (contained(event, cpu_begin, cpu_end) && event.name == "runtime.cpu_collective") return fail("collective_inside_prefetch_local_return");
        if (process.ranks.size() != 1) return fail("prefetch_local_return_rank_not_observed");
        auto cpu = source.timing_interval_ownership(progress.pid, progress.tid, cpu_begin, cpu_end - cpu_begin);
        if (cpu.status != "ready" && cpu.status != "zero_duration") return fail("prefetch_cpu_interval_incomplete");
        result.rank = *process.ranks.begin();
        result.cpu.push_back({ "local_return", std::move(cpu) });
        result.local_return = true;
        return result;
    }
    const auto & check = observations[*result.check];
    if (!progress.progress_ready || check.arg("can_terminate") != (*progress.progress_ready ? "true" : "false"))
        return fail("prefetch_decision_not_observed_or_inconsistent");
    const bool completed = *progress.progress_ready;
    const bool immediate = policy == "best_effort";
    if ((immediate && !completed) || (completed != result.stop.has_value())) return fail("prefetch_stop_does_not_match_decision");
    const Event * max = nullptr;
    const Event * min = nullptr;
    std::string group;
    for (size_t id = 0; id < collectives.rounds.size(); ++id) {
        const auto & round = collectives.rounds[id];
        for (const auto & call : round.calls) {
            const auto & event = observations[call.observation];
            if (!contained(event, progress.timestamp_us, finish)) continue;
            const bool state = event.arg("role") == "prefetch_state_check";
            if (!state && event.arg("role") != "prefetch_completion") return fail("other_collective_inside_prefetch_progress");
            auto reduction = event.arg("reduce_op");
            if (reduction.starts_with("RedOpType.")) reduction.erase(0, 10);
            if (event.arg("request_id") != progress.request_id || event.arg("operation") != "all_reduce" || reduction != (state ? "MAX" : "MIN")
                || event.arg("numel") != (state ? "2" : "1"))
                return fail("prefetch_collective_signature");
            if (!round.issue.empty() || std::ranges::any_of(round.calls, [](const auto & member) { return !member.issue.empty(); }))
                return fail("prefetch_collective_incomplete");
            auto & slot = state ? result.state_round : result.completion_round;
            if (slot) return fail("multiple_prefetch_collective_groups");
            if (result.rank && (*result.rank != call.rank || group != round.group)) return fail("prefetch_collective_group_changed");
            result.rank = call.rank;
            group = round.group;
            slot = id;
            (state ? max : min) = &event;
        }
    }
    if ((immediate && max) || (!immediate && !max) || (completed != (min != nullptr))) return fail("prefetch_collectives_do_not_match_policy_or_decision");
    if (max && !contained(*max, check.ts, end(check))) return fail("state_max_outside_check");
    const auto add = [&](const char * role, uint64_t begin, uint64_t limit) {
        if (limit < begin) {
            result.issue = "prefetch_boundary_order";
            return;
        }
        auto cpu = source.timing_interval_ownership(progress.pid, progress.tid, begin, limit - begin);
        if (cpu.status != "ready" && cpu.status != "zero_duration") result.issue = "prefetch_cpu_interval_incomplete";
        result.cpu.push_back({ role, std::move(cpu) });
    };
    add("before_check", cpu_begin, check.ts);
    if (max) {
        add("check_before_max", check.ts, max->ts);
        add("check_after_max", end(*max), end(check));
    }
    else add("check", check.ts, end(check));
    if (completed) {
        const auto & stop = observations[*result.stop];
        if (stop.ts < end(check) || min->ts < end(stop)) return fail("prefetch_boundary_order");
        if (stop.arg("completed_tokens").empty()) return fail("missing_prefetch_completed_tokens");
        result.completed_tokens = stop.arg_u64("completed_tokens");
        add("after_check", end(check), stop.ts);
        add("stop_call", stop.ts, end(stop));
        add("before_min", end(stop), min->ts);
        add("after_min", end(*min), cpu_end);
    }
    else add("after_check", end(check), cpu_end);
    return result;
}
} // namespace

std::vector<PrefetchControlObservation> observe_prefetch_control(const patch::HiCacheSourceDagIndex & source, const CpuCollectiveObservation & collectives,
                                                                 std::string_view source_policy) {
    std::vector<PrefetchControlObservation> result;
    std::map<std::string, ProcessEvidence> processes;
    const auto & observations = source.graph().runtime_observations();
    for (const auto & event : observations)
        if (event.name == "runtime.hicache.prefetch_check" && event.arg("status") == "returned") processes[event.pid].check_probe = true;
    for (const auto & round : collectives.rounds)
        for (const auto & call : round.calls) processes[observations.at(call.observation).pid].ranks.insert(call.rank);
    for (const auto & fact : source.fact_nodes()) {
        if (fact.fact_role != "prefetch_progress_observed" || fact.phase != "end") continue;
        if (source_policy != "best_effort" && source_policy != "timeout" && source_policy != "wait_complete")
            result.push_back({ .progress_fact = fact.node_id, .issue = "missing_or_unsupported_source_policy" });
        else result.push_back(observe_call(source, collectives, fact, source_policy, processes[fact.pid]));
    }
    return result;
}

PrefetchRequestObservations observe_prefetch_requests(const patch::HiCacheSourceDagIndex & source, const CpuCollectiveObservation & collectives,
                                                      std::string_view policy, const std::set<std::string> * requests) {
    PrefetchRequestObservations grouped;
    for (auto & call : observe_prefetch_control(source, collectives, policy)) {
        const auto * fact = source.fact_node(call.progress_fact);
        if (!fact || (requests && !requests->contains(fact->request_id))) continue;
        if (!call.issue.empty() || !call.rank || call.cpu.empty()) throw std::runtime_error("Request progress observation: " + call.issue);
        grouped[fact->request_id][*call.rank].push_back(std::move(call));
    }
    for (auto & [request, ranks] : grouped)
        for (auto & [rank, calls] : ranks) std::ranges::sort(calls, {}, [](const auto & call) { return call.cpu.front().cpu.interval_start_us; });
    return grouped;
}


std::vector<PrefetchServiceObservation> observe_prefetch_services(const patch::HiCacheSourceDagIndex & source) {
    const auto & observations = source.graph().runtime_observations();
    std::vector<PrefetchServiceObservation> results;
    for (const auto & fact : source.fact_nodes()) {
        if (fact.fact_role != "storage_read_service_observed" || fact.phase != "end") continue;
        PrefetchServiceObservation result{
            .service_fact = fact.node_id,
            .batch = { fact.service_item_count, fact.timestamp_us, fact.timestamp_us + fact.duration_us, {} }
        };
        for (size_t id = 0; id < observations.size(); ++id) {
            const auto & event = observations[id];
            if (event.pid != fact.pid || event.tid != fact.tid || event.ts < fact.timestamp_us || end(event) > result.batch.ready_ts) continue;
            if (event.name == "runtime.hicache.prefetch_read") {
                if (result.read) result.issue = "nonunique_prefetch_read";
                result.read = id;
            }
            else if (event.name == "runtime.hicache.prefetch_publish") result.publications.push_back(id);
        }
        std::ranges::sort(result.publications, [&](size_t a, size_t b) { return observations[a].ts < observations[b].ts; });
        const auto bind = [&] {
            if (!result.issue.empty()) return;
            if (!result.read || result.publications.empty()) {
                result.issue = "missing_prefetch_read_or_publications";
                return;
            }
            const auto & read = observations[*result.read];
            if (read.arg("status") != "returned" || fact.source_page_size == 0 || fact.service_item_count == 0
                || read.arg_u64("page_count") != fact.service_item_count) {
                result.issue = "prefetch_read_geometry_or_status";
                return;
            }
            uint64_t previous_end = end(read), pages = 0, copied = 0;
            bool rejected = false;
            std::optional<uint64_t> previous_tokens;
            for (const auto id : result.publications) {
                const auto & event = observations[id];
                const auto request = event.arg("request_id");
                const auto tokens = event.arg_u64("num_tokens");
                if (request.empty() || (!result.request_id.empty() && request != result.request_id) || event.arg("status") != "returned") {
                    result.issue = "prefetch_publication_identity_or_status";
                    return;
                }
                result.request_id = request;
                const auto accepted = event.arg("accepted");
                if (event.ts < previous_end || tokens == 0 || tokens % fact.source_page_size != 0 || event.arg("completed_tokens").empty()
                    || (accepted != "true" && accepted != "false") || rejected) {
                    result.issue = "prefetch_publication_order_or_geometry";
                    return;
                }
                const auto completed = event.arg_u64("completed_tokens");
                const auto increment = accepted == "true" ? tokens : 0;
                if (completed < increment || (previous_tokens && completed - increment != *previous_tokens)) {
                    result.issue = "prefetch_publication_counter_discontinuity";
                    return;
                }
                previous_tokens = completed;
                previous_end = end(event);
                const auto count = tokens / fact.source_page_size;
                copied = core::checked_add_u64(copied, count, "prefetch observed copy count overflow");
                rejected = accepted == "false";
                if (!rejected) {
                    pages = core::checked_add_u64(pages, count, "prefetch observed page count overflow");
                    result.batch.publications.push_back({ count, end(event), std::nullopt });
                }
            }
            if (copied > fact.service_item_count || (!rejected && pages != fact.service_item_count)) {
                result.issue = "prefetch_service_not_fully_published";
                return;
            }
            result.observed_work = PrefetchBatchExecution{ copied, pages, result.batch.ready_ts, rejected };
            if (rejected) result.issue = "prefetch_service_not_fully_published";
        };
        bind();
        // Partial observations stay inspectable, but never masquerade as a full
        // service timeline for a different target or cancellation decision.
        if (!result.issue.empty()) result.batch.publications.clear();
        results.push_back(std::move(result));
    }
    std::map<size_t, size_t> parents;
    for (const auto & result : results) {
        if (result.read) ++parents[*result.read];
        for (const auto id : result.publications) ++parents[id];
    }
    for (auto & result : results) {
        if ((result.read && parents[*result.read] != 1) || std::ranges::any_of(result.publications, [&](size_t id) { return parents[id] != 1; })) {
            result.issue = "nonunique_prefetch_service_parent";
            result.batch.publications.clear();
            result.observed_work.reset();
        }
    }
    return results;
}
} // namespace markov::trace_graph::modules::hicache
