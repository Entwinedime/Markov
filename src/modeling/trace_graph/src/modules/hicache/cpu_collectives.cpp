#include "markov/trace_graph/modules/hicache/cpu_collectives.hpp"
#include <algorithm>
#include <nlohmann/json.hpp>
#include <set>
#include <tuple>

namespace markov::trace_graph::modules::hicache {
namespace {
using Key = std::tuple<std::string, std::vector<int>, uint64_t>;

bool same_signature(const core::TraceEvent & a, const core::TraceEvent & b) {
    for (const auto field : { "operation", "numel", "dtype", "reduce_op", "src", "group_src", "role", "scheduler_phase", "request_id" })
        if (a.arg(field) != b.arg(field)) return false;
    return true;
}

void observe_local(const patch::HiCacheSourceDagIndex & source, const std::vector<size_t> & workers, CpuCollectiveCall & call) {
    const auto & graph = source.graph();
    const auto & event = graph.runtime_observations()[call.observation];
    if (event.arg("async_op") != "false" || event.arg("status") != "returned" || event.arg_u64("sequence_after") <= event.arg_u64("sequence_before")) {
        call.issue = "not_completed_synchronous_call";
        return;
    }
    const auto operation = event.arg("operation");
    if (operation != "broadcast" && operation != "all_reduce") {
        call.issue = "unsupported_operation";
        return;
    }
    call.cpu = source.timing_interval_ownership(event.pid, event.tid, event.ts, event.dur);
    if (call.cpu.status != "ready") {
        call.issue = "incomplete_cpu_interval";
        return;
    }
    const auto submit_name = operation == "broadcast" ? "c10d::broadcast_" : "c10d::allreduce_";
    for (const auto id : call.cpu.owned_node_ids) {
        const auto & leaf = graph.event_for_node(id);
        if (leaf.name == submit_name) {
            if (call.submission) {
                call.issue = "nonunique_submission";
                return;
            }
            call.submission = id;
        }
        else if (leaf.arg("hicache_control_semantics") != "parent_self_time") {
            call.issue = "unexplained_call_work";
            return;
        }
    }
    if (!call.submission) {
        call.issue = "missing_submission";
        return;
    }
    const auto & submit = graph.event_for_node(*call.submission);
    const auto worker_name = operation == "broadcast" ? "gloo:broadcast" : "gloo:all_reduce";
    const auto first = std::ranges::lower_bound(workers, submit.ts, {}, [&](size_t id) { return graph.event_for_node(id).ts; });
    for (auto it = first; it != workers.end(); ++it) {
        const auto & worker = graph.event_for_node(*it);
        if (worker.ts >= event.ts + event.dur) break;
        if (worker.name != worker_name || worker.pid != event.pid || worker.tid == event.tid || graph.node(*it).gpu_id != graph.node(*call.submission).gpu_id
            || worker.ts + worker.dur > event.ts + event.dur)
            continue;
        if (call.worker) {
            call.issue = "nonunique_worker";
            return;
        }
        call.worker = *it;
    }
    if (!call.worker) call.issue = "missing_worker";
}
} // namespace

CpuCollectiveObservation observe_cpu_collectives(const patch::HiCacheSourceDagIndex & source) {
    CpuCollectiveObservation result;
    const auto & graph = source.graph();
    std::vector<size_t> workers;
    for (const auto & node : graph.nodes()) {
        if (!node.active || !node.is_cpu) continue;
        const auto & event = graph.event_for_node(node.id);
        if (event.name == "gloo:broadcast" || event.name == "gloo:all_reduce") workers.push_back(node.id);
    }
    std::ranges::sort(workers, {}, [&](size_t id) { return graph.event_for_node(id).ts; });
    std::map<Key, size_t> rounds;
    for (size_t i = 0; i < graph.runtime_observations().size(); ++i) {
        const auto & event = graph.runtime_observations()[i];
        if (event.name != "runtime.cpu_collective") continue;
        if (event.arg("group").empty() || event.arg("members").empty() || event.arg("collective_index").empty() || event.arg("rank").empty()) {
            ++result.issues["missing_collective_identity"];
            continue;
        }
        const auto members = nlohmann::json::parse(event.arg("members")).get<std::vector<int>>();
        const Key key{ event.arg("group"), members, event.arg_u64("collective_index") };
        const auto [at, inserted] = rounds.try_emplace(key, result.rounds.size());
        if (inserted) result.rounds.push_back({ .group = std::get<0>(key), .members = members, .index = std::get<2>(key) });
        auto & round = result.rounds[at->second];
        CpuCollectiveCall call{ .observation = i, .rank = std::stoi(event.arg("rank")) };
        observe_local(source, workers, call);
        round.calls.push_back(std::move(call));
    }
    // A plausible interval must not assign the same worker to two calls/groups.
    std::map<size_t, std::vector<CpuCollectiveCall *>> owners;
    for (auto & round : result.rounds)
        for (auto & call : round.calls)
            if (call.issue.empty()) owners[*call.worker].push_back(&call);
    for (const auto & [worker, calls] : owners)
        if (calls.size() > 1)
            for (auto * call : calls) call->issue = "worker_claimed_by_multiple_calls";
    for (auto & round : result.rounds) {
        std::set<int> ranks;
        const auto & first = graph.runtime_observations()[round.calls.front().observation];
        for (const auto & call : round.calls) {
            ranks.insert(call.rank);
            if (!same_signature(first, graph.runtime_observations()[call.observation])) round.issue = "collective_signature_mismatch";
            if (!call.issue.empty()) ++result.issues[call.issue];
        }
        const std::set<int> expected(round.members.begin(), round.members.end());
        if (expected.size() != round.members.size() || ranks != expected || round.calls.size() != expected.size())
            round.issue = "collective_membership_mismatch";
        if (!round.issue.empty()) ++result.issues[round.issue];
    }
    return result;
}

} // namespace markov::trace_graph::modules::hicache
