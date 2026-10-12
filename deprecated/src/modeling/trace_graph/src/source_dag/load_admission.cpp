#include "markov/trace_graph/source_dag/load_admission.hpp"
#include "markov/trace_graph/modules/hicache/model/replay.hpp"

#include <set>
#include <stdexcept>

namespace markov::trace_graph::source_dag {
namespace model = modules::hicache::model;
using modules::hicache::HiCacheFactRole;

namespace {

size_t lookup_owner(const core::TraceEvent & condition, std::span<const model::HiCacheReplayFact> facts, uint64_t boundary_ns) {
    if (condition.arg("request_id").empty() || condition.arg("status") != "returned"
        || (condition.arg("needed") != "true" && condition.arg("needed") != "false"))
        throw std::runtime_error("Load admission lacks a completed request condition");

    const model::HiCacheReplayFact * owner = nullptr;
    bool ambiguous = false;
    for (const auto & item : facts) {
        const auto & fact = item.fact;
        if (item.role != HiCacheFactRole::CacheLookupInput || fact.pid != condition.pid || fact.tid != condition.tid
            || fact.request_id != condition.arg("request_id") || fact.source_ts * 1'000 > boundary_ns)
            continue;

        if (!owner || owner->fact.source_ts < fact.source_ts) {
            owner = &item;
            ambiguous = false;
        }
        else if (owner->fact.source_ts == fact.source_ts) ambiguous = true;
    }

    if (ambiguous) throw std::runtime_error("Load admission has ambiguous lookup inputs");
    if (!owner) throw std::runtime_error("Load admission lacks a preceding lookup of the same request and thread");
    return owner->fact.source_node_id;
}

} // namespace

std::vector<size_t> observe_prelude_load_lookups(std::span<const core::TraceEvent> observations, std::span<const model::HiCacheReplayFact> facts) {
    std::vector<size_t> result;
    for (const auto & event : observations) {
        if (event.name != "runtime.hicache.host_load_check") continue;

        result.push_back(lookup_owner(event, facts, event.ts * 1'000 + event.ts_submicro_ns));
    }
    return result;
}

std::vector<HiCacheLoadBranch> observe_load_branches(std::span<const core::TraceEvent> observations, std::span<const model::HiCacheReplayFact> facts,
                                                     uint64_t begin, uint64_t end) {
    const auto start = [](const auto & event) { return event.ts * 1'000 + event.ts_submicro_ns; };
    const auto stop = [&](const auto & event) { return start(event) + event.dur * 1'000 + event.dur_submicro_ns; };
    std::vector<const core::TraceEvent *> branches, checks;
    for (const auto & event : observations) {
        if (event.ts < begin || stop(event) > end * 1'000) continue;
        if (event.name == "hicache.control.host_load_branch") branches.push_back(&event);
        if (event.name == "runtime.hicache.host_load_check") checks.push_back(&event);
    }
    std::vector<HiCacheLoadBranch> result;
    std::set<const core::TraceEvent *> used;
    for (const auto * branch : branches) {
        const core::TraceEvent * condition = nullptr;
        for (const auto * check : checks) {
            if (check->pid != branch->pid || check->tid != branch->tid || start(*check) < start(*branch) || stop(*check) > stop(*branch)) continue;
            if (condition) throw std::runtime_error("Load branch has multiple conditions");
            condition = check;
        }
        if (!condition || !used.insert(condition).second) throw std::runtime_error("Load branch lacks a unique completed condition");

        result.push_back({ lookup_owner(*condition, facts, start(*branch)), *branch, *condition });
    }
    if (used.size() != checks.size()) throw std::runtime_error("Load condition lacks its complete outer branch");
    return result;
}

} // namespace markov::trace_graph::source_dag
