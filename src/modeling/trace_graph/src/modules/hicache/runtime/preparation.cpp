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

void observe_costs(const core::DagGraph& graph, AllocatorPreparationPlan& plan) {
    std::map<std::string, std::map<std::string, std::vector<uint64_t>>> samples;
    std::map<std::string, std::vector<const Event*>> loads;
    std::map<AllocatorSpecialization, std::vector<const Event*>> compiles;
    bool parallel = true;
    for (const auto& event : graph.runtime_observations()) {
        if (!allocator_event(event)) continue;
        const auto specialization = observed_specialization(event);
        if (!specialization || event.arg("status") != "returned") { parallel = false; continue; }
        if (event.name == "runtime.triton.load") loads[event.pid].push_back(&event);
        if (event.name != "runtime.triton.prepare") continue;
        const auto path = event.arg("path");
        const bool sync = event.arg("execution_mode") == "sync";
        if (sync && (path == "compiled" || path == "disk_cache")) samples[event.pid][path].push_back(event.dur);
        if (sync && path == "compiled") compiles[*specialization].push_back(&event);
        else parallel = false;
    }
    for (const auto& [specialization, events] : compiles) {
        uint64_t begin = 0, end = std::numeric_limits<uint64_t>::max();
        std::set<std::string> ranks;
        for (const auto* event : events) {
            begin = std::max(begin, event->ts);
            end = std::min(end, core::checked_add_u64(event->ts, event->dur, "preparation end overflow"));
            if (!ranks.insert(event->pid).second) parallel = false;
        }
        if (begin >= end) parallel = false;
    }
    plan.source_parallel_compilation = parallel && !compiles.empty();
    for (auto& [pid, events] : loads) {
        std::ranges::sort(events, {}, &Event::ts);
        for (size_t i = 0; i < events.size(); ++i)
            samples[pid][i == 0 ? "first_load" : "variant_load"].push_back(events[i]->dur);
    }
    for (auto& [pid, paths] : samples) for (auto& [path, values] : paths) {
        std::ranges::sort(values);
        const auto left = values[(values.size() - 1) / 2], right = values[values.size() / 2];
        plan.cost_samples[pid][path] = {values.size(), values.front(), left + (right - left) / 2, values.back()};
    }
}

void assign_cache_paths(const std::vector<model::HiCacheAllocatorWorkItem>& calls, AllocatorPreparationPlan& plan) {
    // A batch is shared across ranks, including repeated chunks of the same
    // logical request. Publish its new disk entries only after the whole batch.
    using Batch = std::pair<std::vector<std::string>, size_t>;
    std::map<std::string, std::map<std::vector<std::string>, size_t>> occurrences;
    std::map<Batch, size_t> indexes;
    std::vector<std::vector<size_t>> batches;
    for (size_t i = 0; i < calls.size(); ++i) {
        if (calls[i].request_ids.empty()) continue;
        const Batch key{calls[i].request_ids, occurrences[calls[i].pid][calls[i].request_ids]++};
        const auto [entry, added] = indexes.emplace(key, batches.size());
        if (added) batches.emplace_back();
        batches[entry->second].push_back(i);
    }
    std::set<AllocatorSpecialization> disk;
    bool uncertain = false;
    for (const auto& batch : batches) {
        for (const auto i : batch) {
            auto& item = plan.calls[i];
            if (item.status == "already_prepared") item.path = "memory_cache";
            if (item.status == "required" && item.specialization)
                item.path = disk.contains(*item.specialization) ? "disk_cache" : uncertain ? "unknown" : "compiled";
        }
        for (const auto i : batch) {
            const auto& item = plan.calls[i];
            if (item.status == "required" && item.specialization) disk.insert(*item.specialization);
            if (item.status == "unknown" || item.status == "possibly_required" || item.status == "unsupported_signature") uncertain = true;
        }
    }
}

std::map<Lane, std::vector<std::pair<uint64_t, size_t>>> allocator_gap_anchors(const core::DagGraph& graph) {
    std::map<size_t, std::vector<size_t>> parents;
    std::vector<size_t> devices;
    for (const auto& node : graph.nodes()) {
        if (node.active && !node.is_cpu && graph.event_for_node(node.id).name == "alloc_extend_kernel") {
            devices.push_back(node.id);
            parents[node.id];
        }
    }
    auto frontier = devices;
    for (const auto kind : {core::DagEdgeKind::Correlation, core::DagEdgeKind::Correlation, core::DagEdgeKind::Sequential}) {
        std::set<size_t> wanted(frontier.begin(), frontier.end());
        for (const auto& edge : graph.edges())
            if (edge.active && edge.kind == kind && wanted.contains(edge.dst) && graph.node(edge.src).active)
                parents[edge.dst].push_back(edge.src);
        frontier.clear();
        for (const auto id : wanted) if (parents[id].size() == 1) frontier.push_back(parents[id].front());
    }
    std::map<Lane, std::vector<std::pair<uint64_t, size_t>>> anchors;
    for (const auto device : devices) {
        if (parents[device].size() != 1) continue;
        const auto launch = parents[device].front();
        if (parents[launch].size() != 1) continue;
        const auto enqueue = parents[launch].front();
        if (parents[enqueue].size() != 1) continue;
        const auto before = parents[enqueue].front();
        const auto& event = graph.event_for_node(enqueue);
        const auto& prior = graph.event_for_node(before);
        if (event.name != "Enqueue@alloc_extend_kernel" || !graph.node(enqueue).is_cpu || !graph.node(before).is_cpu
            || event.pid != prior.pid || event.tid != prior.tid) continue;
        anchors[{event.pid, event.tid}].emplace_back(event.ts, before);
    }
    return anchors;
}
}

AllocatorPreparationPlan plan_allocator_preparations(
    const core::DagGraph& graph, const std::vector<model::HiCacheAllocatorWorkItem>& calls) {
    AllocatorPreparationPlan result;
    observe_costs(graph, result);
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
            item.first_load = cache.empty();
            item.status = cache.contains(*item.specialization) ? "already_prepared" :
                uncertain.contains(call.pid) ? "possibly_required" : "required";
            cache.insert(*item.specialization);
        }
        if (call.formal && (item.status == "unknown" || item.status == "possibly_required" || item.status == "unsupported_signature"))
            ++result.blockers[item.status];
        result.calls.push_back(std::move(item));
    }
    assign_cache_paths(calls, result);

    // A cache_extend input is the start of one sequential batch preparation.
    // Restrict each observation to that same thread's batch interval; never use
    // the nearest arbitrary CPU event or a request/workload name as a rule.
    std::map<Lane, std::vector<std::pair<uint64_t, size_t>>> batches;
    std::vector<Lane> call_lanes(calls.size());
    std::vector<Span> call_windows(calls.size(), {0, std::numeric_limits<uint64_t>::max()});
    for (size_t i = 0; i < calls.size(); ++i) {
        if (!calls[i].formal) continue;
        const auto fact = std::ranges::find_if(graph.hicache_fact_events(), [&](const auto& event) {
            return event.index == calls[i].source_fact_id && event.pid == calls[i].pid;
        });
        if (fact == graph.hicache_fact_events().end()) { ++result.blockers["source_batch_boundary_missing"]; continue; }
        batches[{fact->pid, fact->tid}].emplace_back(fact->ts, i);
        call_lanes[i] = {fact->pid, fact->tid};
        call_windows[i].first = fact->ts;
    }
    for (auto& [lane, boundaries] : batches) {
        std::ranges::sort(boundaries);
        for (size_t i = 1; i < boundaries.size(); ++i) call_windows[boundaries[i - 1].second].second = boundaries[i].first;
    }
    std::vector<bool> source_required(calls.size());
    std::vector<bool> source_first_load(calls.size());
    std::vector<std::optional<AllocatorSpecialization>> source_variants(calls.size());
    std::vector<std::string> source_paths(calls.size());
    std::vector<std::vector<Span>> source_spans(calls.size());
    std::map<std::string, uint64_t> first_load;
    for (const auto& event : graph.runtime_observations()) if (allocator_event(event) && event.name == "runtime.triton.load") {
        const auto [entry, added] = first_load.emplace(event.pid, event.ts);
        if (!added) entry->second = std::min(entry->second, event.ts);
    }
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
        if (!observed || event.arg("status") != "returned" || (after != boundaries.end() && end > after->first)
            || (source_variants[i] && source_variants[i] != observed)) {
            ++result.blockers["preparation_observation_not_bound"]; continue;
        }
        source_variants[i] = observed;
        source_spans[i].emplace_back(event.ts, end);
        if (event.name == "runtime.triton.prepare") {
            source_required[i] = true;
            source_paths[i] = event.arg("path");
            if (event.arg("execution_mode") == "async") ++result.blockers["preparation_async_interval_incomplete"];
        }
        if (event.name == "runtime.triton.load" && event.ts == first_load.at(event.pid)) source_first_load[i] = true;
    }
    std::map<size_t, uint64_t> estimated;
    for (size_t i = 0; i < calls.size(); ++i) {
        if (!calls[i].formal) continue;
        result.observed_formal_calls += source_required[i];
        const auto& target = result.calls[i];
        if (target.status == "required" && source_required[i] && target.specialization == source_variants[i]
            && target.first_load == source_first_load[i] && (source_paths[i].empty() || source_paths[i] == target.path)) continue;
        if (target.status == "required") {
            const auto costs = result.cost_samples.find(calls[i].pid);
            if (!result.source_parallel_compilation) ++result.blockers["preparation_compilation_context_uncovered"];
            else if (target.first_load) ++result.blockers["preparation_first_runtime_state_uncovered"];
            else if (costs == result.cost_samples.end() || !costs->second.contains(target.path)
                     || !costs->second.contains("variant_load")) ++result.blockers["new_preparation_cost_uncovered"];
            else estimated[i] = core::checked_add_u64(costs->second.at(target.path).median_us,
                costs->second.at("variant_load").median_us, "predicted preparation cost overflow");
        } else if (target.status != "already_prepared" && target.status != "naive_path") continue;
        auto& spans = removed[call_lanes[i]];
        spans.insert(spans.end(), source_spans[i].begin(), source_spans[i].end());
    }

    std::map<size_t, uint64_t> added;
    if (!estimated.empty()) {
        const auto anchors = allocator_gap_anchors(graph);
        for (const auto& [i, cost] : estimated) {
            std::vector<size_t> matches;
            const auto found = anchors.find(call_lanes[i]);
            if (found != anchors.end()) for (const auto& [ts, node] : found->second)
                if (ts >= call_windows[i].first && ts < call_windows[i].second) matches.push_back(node);
            if (matches.size() != 1 || !added.emplace(matches.front(), cost).second)
                ++result.blockers["preparation_submit_anchor_not_unique"];
        }
    }

    uint64_t required_coverage = 0;
    for (auto& [lane, spans] : removed) {
        spans = interval_union(std::move(spans));
        for (const auto& [begin, end] : spans) required_coverage = core::checked_add_u64(required_coverage, end - begin, "preparation coverage overflow");
    }
    for (const auto& node : graph.nodes()) {
        if (!node.active || !node.is_cpu || (!node.original_cpu_gap_after && !added.contains(node.id))) continue;
        const auto& event = graph.event_for_node(node.id);
        const auto found = removed.find({event.pid, event.tid});
        if (found == removed.end() && !added.contains(node.id)) continue;
        const auto begin = core::checked_add_u64(event.ts, event.dur, "CPU observation end overflow");
        const auto end = core::checked_add_u64(begin, node.original_cpu_gap_after, "CPU gap end overflow");
        uint64_t covered = 0;
        if (found != removed.end()) for (const auto& [left, right] : found->second)
            if (std::max(begin, left) < std::min(end, right)) covered += std::min(end, right) - std::max(begin, left);
        const auto extra = added.contains(node.id) ? added.at(node.id) : uint64_t{0};
        if (!covered && !extra) continue;
        if (node.cpu_gap_after != node.original_cpu_gap_after || graph.scope_gap_duration(node.id)) {
            ++result.blockers["preparation_gap_already_modified"]; continue;
        }
        result.mutation.set_cpu_gaps.push_back({node.id,
            core::checked_add_u64(node.cpu_gap_after - covered, extra, "target preparation gap overflow"),
            "allocator_preparation", "replace observed preparation with target first-use cost; retain residual source time"});
        result.removed_coverage_us = core::checked_add_u64(result.removed_coverage_us, covered, "preparation coverage overflow");
        result.added_cost_us = core::checked_add_u64(result.added_cost_us, extra, "preparation cost overflow");
    }
    if (result.removed_coverage_us != required_coverage) ++result.blockers["preparation_not_fully_in_cpu_gaps"];
    if (!result.blockers.empty() || calls.empty()) {
        result.mutation.set_cpu_gaps.clear();
        result.removed_coverage_us = 0;
        result.added_cost_us = 0;
        result.status = supported.empty() ? "unavailable" : "partial";
    } else result.status = "ready";
    return result;
}

} // namespace markov::trace_graph::modules::hicache::runtime
