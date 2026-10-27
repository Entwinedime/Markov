#include "markov/trace_graph/adapters/frameworks/sglang/allocator_preparation.hpp"
#include "markov/trace_graph/adapters/frameworks/sglang/source_calls.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/modules/hicache/fact.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>
#include <string_view>

namespace markov::trace_graph::adapters::sglang {
namespace {
using Event = core::TraceEvent;
using Span = std::pair<uint64_t, uint64_t>;
using Lane = std::pair<std::string, std::string>;
using Json = nlohmann::json;

// The supported Ascend executor uses the inspected alloc_extend_kernel ABI
// with BLOCK_SIZE=2048. Runtime type and buffer metadata identify that executor
// even when this base only exercised its naive branch and never prepared JIT.
constexpr std::string_view kNpuPagedAllocator = "sglang.srt.hardware_backend.npu.allocator_npu.NPUPagedTokenToKVPoolAllocator";

std::map<Lane, std::vector<std::pair<uint64_t, size_t>>> allocator_gap_anchors(const core::DagGraph & graph) {
    std::map<size_t, std::vector<size_t>> parents;
    std::vector<size_t> devices;
    for (const auto & node : graph.nodes()) {
        if (node.active && !node.is_cpu && graph.event_for_node(node.id).name == "alloc_extend_kernel") {
            devices.push_back(node.id);
            parents[node.id];
        }
    }
    auto frontier = devices;
    for (const auto kind : { core::DagEdgeKind::Correlation, core::DagEdgeKind::Correlation, core::DagEdgeKind::Sequential }) {
        std::set<size_t> wanted(frontier.begin(), frontier.end());
        for (const auto & edge : graph.edges())
            if (edge.active && edge.kind == kind && wanted.contains(edge.dst) && graph.node(edge.src).active) parents[edge.dst].push_back(edge.src);
        frontier.clear();
        for (const auto id : wanted)
            if (parents[id].size() == 1) frontier.push_back(parents[id].front());
    }
    std::map<Lane, std::vector<std::pair<uint64_t, size_t>>> anchors;
    for (const auto device : devices) {
        if (parents[device].size() != 1) continue;
        const auto launch = parents[device].front();
        if (parents[launch].size() != 1) continue;
        const auto enqueue = parents[launch].front();
        if (parents[enqueue].size() != 1) continue;
        const auto before = parents[enqueue].front();
        const auto & event = graph.event_for_node(enqueue);
        const auto & prior = graph.event_for_node(before);
        if (event.name != "Enqueue@alloc_extend_kernel" || !graph.node(enqueue).is_cpu || !graph.node(before).is_cpu || event.pid != prior.pid
            || event.tid != prior.tid)
            continue;
        anchors[{ event.pid, event.tid }].emplace_back(event.ts, before);
    }
    return anchors;
}
} // namespace

bool is_allocator_preparation_event(const Event & event) {
    return event.arg("kernel") == "alloc_extend_kernel" || event.arg("kernel") == "alloc_extend_kernel_aiv";
}

std::optional<adapters::sglang::AllocatorSpecialization> allocator_preparation_specialization(const Event & event) {
    // This is the installed NPU allocator's actual signature, not a kernel-name
    // guess. The generic SGLang Triton allocator has different constexpr inputs.
    const auto constants = Json::parse(event.arg("constants", "{}"));
    const auto signature = Json::parse(event.arg("signature", "{}"));
    const auto properties = Json::parse(event.arg("argument_properties", "{}"));
    for (const auto * name : { "pre_lens_ptr", "seq_lens_ptr", "last_loc_ptr", "free_page_ptr", "out_indices" })
        if (signature.value(name, "") != "*i64") return std::nullopt;
    for (const auto * name : { "bs_upper", "page_size", "max_num_extend_tokens", "BLOCK_SIZE" })
        if (!constants.contains(name) || !constants[name].is_number_unsigned()) return std::nullopt;
    if (constants["BLOCK_SIZE"] != 2'048 || !properties.contains("tt.divisibility")) return std::nullopt;
    const auto aligned = properties.at("tt.divisibility").get<std::vector<int>>();
    for (const int pointer : { 0, 1, 2, 4 })
        if (std::ranges::find(aligned, pointer) == aligned.end()) return std::nullopt;
    return AllocatorSpecialization{ constants["page_size"],
                                    constants["bs_upper"],
                                    constants["max_num_extend_tokens"],
                                    std::ranges::find(aligned, 3) == aligned.end() ? uint64_t{ 8 } : uint64_t{ 0 } };
}

std::vector<Span> merge_allocator_preparation_intervals(std::vector<Span> intervals) {
    std::ranges::sort(intervals);
    std::vector<Span> result;
    for (const auto & span : intervals) {
        if (!result.empty() && span.first <= result.back().second) result.back().second = std::max(result.back().second, span.second);
        else result.push_back(span);
    }
    return result;
}

AllocatorPreparationObservation observe_allocator_preparations(const core::DagGraph & graph) {
    AllocatorPreparationObservation result;
    // Source batches bound observations even when target execution has not yet
    // reached them. Never infer an owner from the nearest arbitrary CPU event.
    std::map<Lane, std::vector<std::pair<uint64_t, size_t>>> batches;
    std::vector<std::string> batch_requests;
    for (const auto & event : graph.hicache_fact_events()) {
        if (event.arg("fact").empty() || modules::hicache::parse_hicache_fact_metadata(event).role != "cache_extend_input") continue;
        batches[{ event.pid, event.tid }].emplace_back(event.ts, result.calls.size());
        batch_requests.push_back(event.arg("request_ids"));
        result.calls.push_back(
            { .source_fact_id = event.index, .pid = event.pid, .tid = event.tid, .begin_us = event.ts, .end_us = std::numeric_limits<uint64_t>::max() });
    }
    for (auto & [lane, boundaries] : batches) {
        std::ranges::sort(boundaries);
        for (size_t i = 1; i < boundaries.size(); ++i) result.calls[boundaries[i - 1].second].end_us = boundaries[i].first;
    }
    for (const auto & event : graph.runtime_observations()) {
        if (event.name != "runtime.hicache.extend_allocation") continue;
        const auto lane = batches.find({ event.pid, event.tid });
        if (lane == batches.end()) continue;
        const auto & boundaries = lane->second;
        const auto after = std::ranges::upper_bound(boundaries, event.ts, {}, &std::pair<uint64_t, size_t>::first);
        if (after == boundaries.begin()) continue;

        const auto call_index = std::prev(after)->second;
        auto & call = result.calls[call_index];
        const auto end_us = core::checked_add_u64(event.ts, event.dur, "extend allocation observation end overflow");
        if (event.arg("status") != "returned" || Json::parse(event.arg("request_ids")) != Json::parse(batch_requests[call_index]) || end_us > call.end_us) {
            ++call.issues["allocator_observation_not_bound"];
            continue;
        }
        // A measured call owns preparation only inside its exact envelope.
        // Older captures without this diagnostic retain the batch boundary;
        // no guessed envelope is manufactured from nearby kernel names.
        call.begin_us = event.ts;
        call.end_us = end_us;
        if (event.has_arg_key_hint("prefix_lens_tokens") && event.has_arg_key_hint("sequence_lens_tokens")) {
            const auto ids = Json::parse(event.arg("request_ids")).get<std::vector<std::string>>();
            const auto prefixes = Json::parse(event.arg("prefix_lens_tokens")).get<std::vector<uint64_t>>();
            const auto sequences = Json::parse(event.arg("sequence_lens_tokens")).get<std::vector<uint64_t>>();
            if (ids.size() != prefixes.size() || ids.size() != sequences.size()) {
                ++call.issues["allocator_lengths_incomplete"];
                continue;
            }
            call.requests.clear();
            for (size_t i = 0; i < ids.size(); ++i) {
                if (prefixes[i] > sequences[i]) {
                    ++call.issues["allocator_lengths_invalid"];
                    call.requests.clear();
                    break;
                }
                call.requests.push_back({ ids[i], sequences[i], prefixes[i] });
            }
        }
        if (event.arg("allocator_implementation") == kNpuPagedAllocator && event.arg("free_page_dtype") == "torch.int64"
            && event.arg("free_page_stride_elements") == "1") {
            result.supported_pids.insert(event.pid);
        }
    }
    // alloc_for_extend also constructs last_loc and writes the request table.
    // Prefer the allocator's own envelope when it was captured, so preparation
    // cannot acquire unrelated work from those surrounding operations.
    std::set<size_t> refined_calls;
    for (const auto & event : graph.runtime_observations()) {
        if (event.name != "runtime.hicache.extend_allocator") continue;
        const auto lane = batches.find({ event.pid, event.tid });
        if (lane == batches.end()) continue;
        const auto after = std::ranges::upper_bound(lane->second, event.ts, {}, &std::pair<uint64_t, size_t>::first);
        if (after == lane->second.begin()) continue;

        const auto call_index = std::prev(after)->second;
        auto & call = result.calls[call_index];
        const auto end_us = core::checked_add_u64(event.ts, event.dur, "allocator call observation end overflow");
        if (event.arg("status") != "returned" || event.ts < call.begin_us || end_us > call.end_us) {
            ++call.issues["allocator_call_observation_not_bound"];
            continue;
        }
        // An unsuccessful attempt returns before preparing this kernel. Keep
        // the raw attempt for structure ownership; do not invent JIT service.
        if (event.arg("allocated") == "false") continue;
        if (event.arg("allocated") != "true" || !refined_calls.insert(call_index).second) {
            ++call.issues["allocator_call_observation_not_unique"];
            continue;
        }

        call.begin_us = event.ts;
        call.end_us = end_us;
        call.allocator_observation = static_cast<size_t>(&event - graph.runtime_observations().data());
        call.allocator_work = adapters::sglang::observed_allocator_work(event);
        if (event.has_arg_key_hint("prefix_dtype")) call.prefix_dtype = event.arg("prefix_dtype");
        // TraceEvent normalizes JSON null to an empty string; zero remains "0"
        // and means the caller did supply an exact (empty) page count.
        if (const auto supplied = event.find_arg("supplied_new_pages")) call.page_count_supplied = !supplied->empty();
        if (call.allocator_work) {
            auto & work = *call.allocator_work;
            work.source_fact_id = call.source_fact_id;
            work.requests = call.requests;
            uint64_t tokens = 0, pages = 0;
            for (const auto & request : work.requests) {
                const auto regions = adapters::sglang::extend_page_regions(request.sequence_tokens, request.prefix_tokens, work.page_size);
                tokens = core::checked_add_u64(tokens, request.sequence_tokens - request.prefix_tokens, "Observed extend tokens overflow");
                pages = core::checked_add_u64(pages, regions.new_full_pages + (regions.new_tail_tokens != 0), "Observed extend pages overflow");
            }
            if (work.requests.size() != work.batch_size || tokens != work.extend_tokens || pages != work.allocated_pages) {
                ++call.issues["allocator_work_observation_mismatch"];
                call.allocator_work.reset();
            }
        }
        if (event.arg("allocator_implementation") == kNpuPagedAllocator && event.arg("free_page_dtype") == "torch.int64"
            && event.arg("free_page_stride_elements") == "1")
            result.supported_pids.insert(event.pid);
    }
    std::map<std::string, uint64_t> first_load;
    for (const auto & event : graph.runtime_observations()) {
        if (!is_allocator_preparation_event(event)) continue;
        if (event.name == "runtime.triton.prepare" && allocator_preparation_specialization(event)) result.supported_pids.insert(event.pid);
        if (event.name == "runtime.triton.load") {
            const auto [entry, added] = first_load.emplace(event.pid, event.ts);
            if (!added) entry->second = std::min(entry->second, event.ts);
        }
    }
    for (const auto & event : graph.runtime_observations()) {
        if (!is_allocator_preparation_event(event)) continue;
        const auto lane = batches.find({ event.pid, event.tid });
        if (lane == batches.end()) continue;
        const auto & boundaries = lane->second;
        const auto after = std::ranges::upper_bound(boundaries, event.ts, {}, &std::pair<uint64_t, size_t>::first);
        if (after == boundaries.begin()) continue; // Prelude supplies samples, not a formal cost.
        auto & call = result.calls[std::prev(after)->second];
        if (event.ts < call.begin_us) continue;
        const auto end = core::checked_add_u64(event.ts, event.dur, "preparation observation end overflow");
        const auto observed = allocator_preparation_specialization(event);
        if (!observed || event.arg("status") != "returned" || end > call.end_us || (call.specialization && call.specialization != observed)) {
            ++call.issues["preparation_observation_not_bound"];
            continue;
        }
        call.specialization = observed;
        call.intervals.emplace_back(event.ts, end);
        if (event.name == "runtime.triton.prepare") {
            call.required = true;
            call.path = event.arg("path");
            if (event.arg("execution_mode") == "async") ++call.issues["preparation_async_interval_incomplete"];
        }
        if (event.name == "runtime.triton.load" && event.ts == first_load.at(event.pid)) call.first_load = true;
    }
    const auto anchors = allocator_gap_anchors(graph);
    for (auto & call : result.calls) {
        call.intervals = merge_allocator_preparation_intervals(std::move(call.intervals));
        for (const auto & [begin, end] : call.intervals)
            call.service_duration_us = core::checked_add_u64(call.service_duration_us,
                                                             graph.cpu_service_cost().duration({ call.pid, call.tid }, begin, end),
                                                             "preparation service overflow");

        std::vector<size_t> matches;
        const auto lane = anchors.find({ call.pid, call.tid });
        if (lane != anchors.end())
            for (const auto & [ts, node] : lane->second)
                if (ts >= call.begin_us && ts < call.end_us) matches.push_back(node);
        if (matches.size() == 1) call.submit_gap_node = matches.front();
    }
    return result;
}

} // namespace markov::trace_graph::adapters::sglang
