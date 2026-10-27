#include "markov/trace_graph/modules/hicache/diagnostics/allocator_preparation.hpp"
#include "markov/trace_graph/core/numeric.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <string_view>

namespace markov::trace_graph::modules::hicache::diagnostics {
namespace {
using Event = core::TraceEvent;
using Span = std::pair<uint64_t, uint64_t>;
using Lane = std::pair<std::string, std::string>;
using adapters::sglang::AllocatorSpecialization;

uint64_t observed_service(const core::DagGraph & graph, const Event & event) {
    return graph.cpu_service_cost().duration({ event.pid, event.tid },
                                             event.ts,
                                             core::checked_add_u64(event.ts, event.dur, "preparation observation end overflow"));
}

std::optional<bool> observe_compilation_context(const core::DagGraph & graph) {
    std::map<AllocatorSpecialization, std::vector<const Event *>> compiles;
    bool parallel = true;
    for (const auto & event : graph.runtime_observations()) {
        if (!adapters::sglang::is_allocator_preparation_event(event)) continue;
        const auto specialization = adapters::sglang::allocator_preparation_specialization(event);
        if (!specialization || event.arg("status") != "returned") {
            parallel = false;
            continue;
        }
        if (event.name != "runtime.triton.prepare") continue;
        const auto path = event.arg("path");
        const bool sync = event.arg("execution_mode") == "sync";
        if (sync && path == "compiled") compiles[*specialization].push_back(&event);
        else parallel = false;
    }
    for (const auto & [specialization, events] : compiles) {
        uint64_t begin = 0, end = std::numeric_limits<uint64_t>::max();
        std::set<std::string> ranks;
        for (const auto * event : events) {
            begin = std::max(begin, event->ts);
            end = std::min(end, core::checked_add_u64(event->ts, event->dur, "preparation end overflow"));
            if (!ranks.insert(event->pid).second) parallel = false;
        }
        // One compiler interval cannot demonstrate cross-rank overlap.
        if (ranks.size() < 2 || begin >= end) parallel = false;
    }
    return compiles.empty() ? std::nullopt : std::optional<bool>{ parallel };
}

auto observe_static_cost_samples(const core::DagGraph & graph) {
    std::map<std::string, std::map<std::string, std::vector<uint64_t>>> samples;
    std::map<std::string, std::vector<const Event *>> loads;
    for (const auto & event : graph.runtime_observations()) {
        if (!adapters::sglang::is_allocator_preparation_event(event) || !adapters::sglang::allocator_preparation_specialization(event)
            || event.arg("status") != "returned")
            continue;
        if (event.name == "runtime.triton.load") loads[event.pid].push_back(&event);
        const auto path = event.arg("path");
        if (event.name == "runtime.triton.prepare" && event.arg("execution_mode") == "sync" && (path == "compiled" || path == "disk_cache"))
            samples[event.pid][path].push_back(observed_service(graph, event));
    }
    for (auto & [pid, events] : loads) {
        std::ranges::sort(events, {}, &Event::ts);
        for (size_t i = 0; i < events.size(); ++i) samples[pid][i == 0 ? "first_load" : "variant_load"].push_back(observed_service(graph, *events[i]));
    }
    std::map<std::string, std::map<std::string, PreparationCostSamples>> result;
    for (auto & [pid, paths] : samples)
        for (auto & [path, values] : paths) {
            std::ranges::sort(values);
            const auto left = values[(values.size() - 1) / 2], right = values[values.size() / 2];
            result[pid][path] = { values.size(), values.front(), left + (right - left) / 2, values.back() };
        }
    return result;
}

AllocatorPreparationPlan predict_static_allocator_preparations(const core::DagGraph & graph, const adapters::sglang::AllocatorPreparationObservation & source,
                                                               const std::vector<model::HiCacheAllocatorWorkItem> & calls) {
    auto work = adapters::sglang::plan_allocator_work(source.supported_pids, calls);
    AllocatorPreparationPlan result{ .status = std::move(work.status), .calls = std::move(work.calls), .blockers = std::move(work.blockers) };
    result.source_parallel_compilation = observe_compilation_context(graph);
    result.cost_samples = observe_static_cost_samples(graph);
    for (size_t i = 0; i < calls.size(); ++i) {
        if (!calls[i].formal || calls[i].phase == model::HiCacheAllocatorPhase::Decode) continue;
        const auto found =
            std::ranges::find_if(source.calls, [&](const auto & call) { return call.source_fact_id == calls[i].source_fact_id && call.pid == calls[i].pid; });
        if (found == source.calls.end()) {
            ++result.blockers["source_batch_boundary_missing"];
            continue;
        }
        for (const auto & [issue, count] : found->issues) result.blockers[issue] += count;
        result.observed_formal_calls += found->required;
        const auto & target = result.calls[i];
        AllocatorPreparationCpuCost cpu{ .source_call = static_cast<size_t>(found - source.calls.begin()) };
        if (target.status == "required" && found->required && target.specialization == found->specialization && target.first_load == found->first_load
            && (found->path.empty() || found->path == target.path)) {
            cpu.retain_source = true;
            cpu.duration_us = found->service_duration_us;
            result.cpu_costs.push_back(cpu);
            continue;
        }
        if (target.status == "required") {
            const auto costs = result.cost_samples.find(calls[i].pid);
            if (result.source_parallel_compilation != true) ++result.blockers["preparation_compilation_context_uncovered"];
            else if (target.first_load) ++result.blockers["preparation_first_runtime_state_uncovered"];
            else if (costs == result.cost_samples.end() || !costs->second.contains(target.path) || !costs->second.contains("variant_load"))
                ++result.blockers["new_preparation_cost_uncovered"];
            else
                cpu.duration_us = core::checked_add_u64(costs->second.at(target.path).median_us,
                                                        costs->second.at("variant_load").median_us,
                                                        "predicted preparation cost overflow");
        }
        else if (target.status != "already_prepared" && target.status != "naive_path") continue;
        result.cpu_costs.push_back(cpu);
    }
    if (!result.blockers.empty() || calls.empty()) {
        result.cpu_costs.clear();
        result.status = source.supported_pids.empty() ? "unavailable" : "partial";
    }
    else result.status = "ready";
    return result;
}

} // namespace

AllocatorPreparationPlan plan_allocator_preparations(const core::DagGraph & graph, const std::vector<model::HiCacheAllocatorWorkItem> & calls) {
    const auto source = adapters::sglang::observe_allocator_preparations(graph);
    auto result = predict_static_allocator_preparations(graph, source, calls);
    std::map<Lane, std::vector<Span>> removed;
    std::map<size_t, uint64_t> added;
    for (const auto & cost : result.cpu_costs) {
        if (cost.retain_source) continue;
        const auto & call = source.calls.at(cost.source_call);
        auto & spans = removed[{ call.pid, call.tid }];
        spans.insert(spans.end(), call.intervals.begin(), call.intervals.end());
        if (cost.duration_us && (!call.submit_gap_node || !added.emplace(*call.submit_gap_node, cost.duration_us).second))
            ++result.blockers["preparation_submit_anchor_not_unique"];
    }

    uint64_t required_coverage = 0;
    for (auto & [lane, spans] : removed) {
        spans = adapters::sglang::merge_allocator_preparation_intervals(std::move(spans));
        for (const auto & [begin, end] : spans) required_coverage = core::checked_add_u64(required_coverage, end - begin, "preparation coverage overflow");
    }
    for (const auto & node : graph.nodes()) {
        if (!node.active || !node.is_cpu || (!node.original_cpu_gap_after && !added.contains(node.id))) continue;
        const auto & event = graph.event_for_node(node.id);
        const auto found = removed.find({ event.pid, event.tid });
        if (found == removed.end() && !added.contains(node.id)) continue;
        const auto begin = core::checked_add_u64(event.ts, event.dur, "CPU observation end overflow");
        const auto end = core::checked_add_u64(begin, node.original_cpu_gap_after, "CPU gap end overflow");
        uint64_t covered = 0;
        if (found != removed.end())
            for (const auto & [left, right] : found->second)
                if (std::max(begin, left) < std::min(end, right)) covered += std::min(end, right) - std::max(begin, left);
        const auto extra = added.contains(node.id) ? added.at(node.id) : uint64_t{ 0 };
        if (!covered && !extra) continue;
        if (node.cpu_gap_after != node.original_cpu_gap_after || graph.scope_gap_duration(node.id)) {
            ++result.blockers["preparation_gap_already_modified"];
            continue;
        }
        result.mutation.set_cpu_gaps.push_back({ node.id,
                                                 core::checked_add_u64(node.cpu_gap_after - covered, extra, "target preparation gap overflow"),
                                                 "allocator_preparation",
                                                 "replace observed preparation with target first-use cost; retain residual source time" });
        result.removed_coverage_us = core::checked_add_u64(result.removed_coverage_us, covered, "preparation coverage overflow");
        result.added_cost_us = core::checked_add_u64(result.added_cost_us, extra, "preparation cost overflow");
    }
    if (result.removed_coverage_us != required_coverage) ++result.blockers["preparation_not_fully_in_cpu_gaps"];
    if (!result.blockers.empty() || calls.empty()) {
        result.mutation.set_cpu_gaps.clear();
        result.removed_coverage_us = 0;
        result.added_cost_us = 0;
        result.status = source.supported_pids.empty() ? "unavailable" : "partial";
    }
    else result.status = "ready";
    return result;
}

} // namespace markov::trace_graph::modules::hicache::diagnostics
