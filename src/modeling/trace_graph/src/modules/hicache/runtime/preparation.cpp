#include "markov/trace_graph/modules/hicache/runtime/preparation.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include <algorithm>
#include <bit>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>

namespace markov::trace_graph::modules::hicache::runtime {
namespace {
using Event = core::TraceEvent;
using Span = std::pair<uint64_t, uint64_t>;
using Lane = std::pair<std::string, std::string>;
using Json = nlohmann::json;

bool allocator_event(const Event& event) {
    return event.arg("kernel") == "alloc_extend_kernel" || event.arg("kernel") == "alloc_extend_kernel_aiv";
}

std::optional<AllocatorSpecialization> observed_specialization(const Event& event) {
    // This is the installed NPU allocator's actual signature, not a kernel-name
    // guess. The generic SGLang Triton allocator has different constexpr inputs.
    const auto constants = Json::parse(event.arg("constants", "{}"));
    const auto signature = Json::parse(event.arg("signature", "{}"));
    const auto properties = Json::parse(event.arg("argument_properties", "{}"));
    for (const auto* name : {"pre_lens_ptr", "seq_lens_ptr", "last_loc_ptr", "free_page_ptr", "out_indices"})
        if (signature.value(name, "") != "*i64") return std::nullopt;
    for (const auto* name : {"bs_upper", "page_size", "max_num_extend_tokens", "BLOCK_SIZE"})
        if (!constants.contains(name) || !constants[name].is_number_unsigned()) return std::nullopt;
    if (constants["BLOCK_SIZE"] != 2048 || !properties.contains("tt.divisibility")) return std::nullopt;
    const auto aligned = properties.at("tt.divisibility").get<std::vector<int>>();
    for (const int pointer : {0, 1, 2, 4}) if (std::ranges::find(aligned, pointer) == aligned.end()) return std::nullopt;
    return AllocatorSpecialization{constants["page_size"], constants["bs_upper"], constants["max_num_extend_tokens"],
        std::ranges::find(aligned, 3) == aligned.end() ? uint64_t{8} : uint64_t{0}};
}

std::vector<Span> interval_union(std::vector<Span> intervals) {
    std::ranges::sort(intervals);
    std::vector<Span> result;
    for (const auto& span : intervals) {
        if (!result.empty() && span.first <= result.back().second) result.back().second = std::max(result.back().second, span.second);
        else result.push_back(span);
    }
    return result;
}
}

AllocatorPreparationPlan plan_allocator_preparations(
    const core::DagGraph& graph, const std::vector<model::HiCacheAllocatorWorkItem>& calls) {
    AllocatorPreparationPlan result;
    std::map<std::string, std::set<AllocatorSpecialization>> prepared;
    std::set<std::string> supported, uncertain;
    for (const auto& event : graph.runtime_observations()) {
        if (event.name == "runtime.triton.prepare" && allocator_event(event) && observed_specialization(event)) supported.insert(event.pid);
    }
    for (const auto& call : calls) {
        AllocatorPreparation item;
        if (!supported.contains(call.pid)) item.status = "unsupported_signature";
        else if (call.allocated_pages >= 200) item.status = "naive_path";
        else if (!call.free_index_offset || !call.extend_tokens || !call.batch_size
                 || call.extend_tokens > (uint64_t{1} << 63) || call.batch_size > (uint64_t{1} << 63)) {
            item.status = "unknown";
            uncertain.insert(call.pid);
        } else {
            item.specialization = AllocatorSpecialization{call.page_size, std::bit_ceil(call.batch_size),
                std::bit_ceil(call.extend_tokens), (*call.free_index_offset % 2) * 8};
            auto& cache = prepared[call.pid];
            item.status = cache.contains(*item.specialization) ? "already_prepared" :
                uncertain.contains(call.pid) ? "possibly_required" : "required";
            cache.insert(*item.specialization);
        }
        if (call.formal && (item.status == "unknown" || item.status == "possibly_required" || item.status == "unsupported_signature"))
            ++result.blockers[item.status];
        result.calls.push_back(std::move(item));
    }

    // A cache_extend input is the start of one sequential batch preparation.
    // Restrict each observation to that same thread's batch interval; never use
    // the nearest arbitrary CPU event or a request/workload name as a rule.
    std::map<Lane, std::vector<std::pair<uint64_t, size_t>>> batches;
    for (size_t i = 0; i < calls.size(); ++i) {
        if (!calls[i].formal) continue;
        const auto fact = std::ranges::find_if(graph.hicache_fact_events(), [&](const auto& event) {
            return event.index == calls[i].source_fact_id && event.pid == calls[i].pid;
        });
        if (fact == graph.hicache_fact_events().end()) { ++result.blockers["source_batch_boundary_missing"]; continue; }
        batches[{fact->pid, fact->tid}].emplace_back(fact->ts, i);
    }
    for (auto& [lane, boundaries] : batches) std::ranges::sort(boundaries);
    std::vector<bool> source_required(calls.size());
    std::map<Lane, std::vector<Span>> removed;
    for (const auto& event : graph.runtime_observations()) {
        if (!allocator_event(event)) continue;
        const Lane lane{event.pid, event.tid};
        const auto lane_batches = batches.find(lane);
        if (lane_batches == batches.end()) continue;
        const auto& boundaries = lane_batches->second;
        auto after = std::ranges::upper_bound(boundaries, event.ts, {}, &std::pair<uint64_t, size_t>::first);
        if (after == boundaries.begin()) continue; // Prelude is context, not a formal cost.
        const auto i = std::prev(after)->second;
        const auto end = core::checked_add_u64(event.ts, event.dur, "preparation observation end overflow");
        const auto observed = observed_specialization(event);
        if (!observed || event.arg("status") != "returned" || (after != boundaries.end() && end > after->first)) {
            ++result.blockers["preparation_observation_not_bound"]; continue;
        }
        if (event.name == "runtime.triton.prepare") source_required[i] = true;
        const auto& target = result.calls[i];
        if (target.status == "already_prepared" || target.status == "naive_path") removed[lane].emplace_back(event.ts, end);
        else if (target.status != "required" || target.specialization != observed) ++result.blockers["target_preparation_cost_uncovered"];
    }
    for (size_t i = 0; i < calls.size(); ++i) {
        if (!calls[i].formal) continue;
        result.observed_formal_calls += source_required[i];
        if (result.calls[i].status == "required" && !source_required[i]) ++result.blockers["new_preparation_cost_uncovered"];
    }

    uint64_t required_coverage = 0;
    for (auto& [lane, spans] : removed) {
        spans = interval_union(std::move(spans));
        for (const auto& [begin, end] : spans) required_coverage = core::checked_add_u64(required_coverage, end - begin, "preparation coverage overflow");
    }
    for (const auto& node : graph.nodes()) {
        if (!node.active || !node.is_cpu || !node.original_cpu_gap_after) continue;
        const auto& event = graph.event_for_node(node.id);
        const auto found = removed.find({event.pid, event.tid});
        if (found == removed.end()) continue;
        const auto begin = core::checked_add_u64(event.ts, event.dur, "CPU observation end overflow");
        const auto end = core::checked_add_u64(begin, node.original_cpu_gap_after, "CPU gap end overflow");
        uint64_t covered = 0;
        for (const auto& [left, right] : found->second)
            if (std::max(begin, left) < std::min(end, right)) covered += std::min(end, right) - std::max(begin, left);
        if (!covered) continue;
        if (node.cpu_gap_after != node.original_cpu_gap_after || graph.scope_gap_duration(node.id)) {
            ++result.blockers["preparation_gap_already_modified"]; continue;
        }
        result.mutation.set_cpu_gaps.push_back({node.id, node.cpu_gap_after - covered,
            "allocator_preparation", "target variant already prepared or non-Triton allocation; retain uncovered source time"});
        result.removed_coverage_us = core::checked_add_u64(result.removed_coverage_us, covered, "preparation coverage overflow");
    }
    if (result.removed_coverage_us != required_coverage) ++result.blockers["preparation_not_fully_in_cpu_gaps"];
    if (!result.blockers.empty() || calls.empty()) {
        result.mutation.set_cpu_gaps.clear();
        result.removed_coverage_us = 0;
        result.status = supported.empty() ? "unavailable" : "partial";
    } else result.status = "ready";
    return result;
}

} // namespace markov::trace_graph::modules::hicache::runtime
