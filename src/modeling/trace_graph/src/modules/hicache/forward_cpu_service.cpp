#include "markov/trace_graph/modules/hicache/forward_cpu_service.hpp"
#include "markov/trace_graph/adapters/frameworks/sglang/allocator_preparation.hpp"
#include "markov/trace_graph/adapters/frameworks/sglang/hicache_observations.hpp"
#include "markov/trace_graph/modules/hicache/layer_waits.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <tuple>

namespace markov::trace_graph::modules::hicache {

std::vector<ForwardCpuAdjustment> plan_forward_cpu_service(const core::DagGraph & graph, const nlohmann::json & steps, const nlohmann::json & comparison) {
    std::set<size_t> reserved;
    const adapters::sglang::HiCacheSourceIndex source(graph);
    const auto waits = observe_hicache_layer_waits(source);
    if (waits.status != "ready") {
        nlohmann::json details{
            { "issues", waits.issues }
        };
        for (const auto & call : waits.calls) {
            if (call.issue.empty()) continue;

            details["first_call"] = {
                {     "request_id",                call.request_id },
                {          "phase",                     call.phase },
                {          "layer",                     call.layer },
                {       "position",                  call.position },
                {        "enabled",                   call.enabled },
                {       "start_ns",                  call.start_ns },
                {         "end_ns",                    call.end_ns },
                {          "issue",                     call.issue },
                {     "cpu_reason",                call.cpu.reason },
                { "cpu_node_count", call.cpu.owned_node_ids.size() }
            };
            for (const auto & [role, id] : {
                     std::pair{      "before",      call.before },
                     std::pair{       "after",       call.after },
                     std::pair{  "submission",  call.submission },
                     std::pair{      "worker",      call.worker },
                     std::pair{ "device_wait", call.device_wait },
                     std::pair{      "record",      call.record }
            }) {
                if (!id) continue;

                const auto & event = graph.event_for_node(*id);
                details["first_call"][role] = {
                    {     "id",        *id },
                    {   "name", event.name },
                    {  "ts_us",   event.ts },
                    { "dur_us",  event.dur }
                };
            }
            break;
        }
        throw std::runtime_error("CPU normalization needs complete layer ownership: " + details.dump());
    }
    reserved.insert(waits.cpu_node_ids.begin(), waits.cpu_node_ids.end());
    for (const auto & call : waits.calls) {
        for (const auto id : { call.before, call.after, call.submission, call.worker })
            if (id) reserved.insert(*id);
        for (const auto & slice : call.cpu.owned_gap_slices) reserved.insert(slice.owner_node_id);
    }
    for (const auto & call : adapters::sglang::observe_allocator_preparations(graph).calls)
        if (call.submit_gap_node) reserved.insert(*call.submit_gap_node);
    using Key = std::tuple<std::string, int, std::string, size_t>;
    std::map<Key, double> reductions;
    std::map<Key, int64_t> layer_reductions;
    for (const auto & row : comparison.at("rows")) {
        Key key{ row.at("request_id"), row.at("rank"), row.at("phase"), row.value("forward_ordinal", size_t{ 0 }) };
        if (!reductions.emplace(key, row.at("delta").at("outside_sync_cpu_us")).second) throw std::runtime_error("duplicate CPU budget identity");
        layer_reductions.emplace(key, row.value("layer_reduction_us", int64_t{ 0 }));
    }
    std::vector<ForwardCpuAdjustment> changes;
    std::map<std::pair<size_t, bool>, bool> owners;
    std::map<std::pair<std::string, std::string>, std::vector<uint64_t>> lane_ends;
    for (const auto & step : steps.at("rows")) {
        const std::string name = step.at("name");
        const Key key{ step.at("request_id"), step.at("identity").at("tp_rank"), name.substr(0, name.find(' ')), step.value("forward_ordinal", size_t{ 0 }) };
        const auto delta = reductions.at(key);
        if (delta < 0 || !std::isfinite(delta)) throw std::runtime_error("Forward CPU allocation requires a nonnegative measured reduction");
        const auto ordinary = static_cast<__int128>(std::llround(delta)) - layer_reductions.at(key);
        if (ordinary < 0 || ordinary > std::numeric_limits<uint64_t>::max())
            throw std::runtime_error("Layer CPU allocation leaves an invalid ordinary forward budget");
        const auto reduction = static_cast<uint64_t>(ordinary);
        const auto begin = step.at("start_ns").get<uint64_t>() / 1'000;
        const auto end = step.at("end_ns").get<uint64_t>() / 1'000;
        const auto pid = std::to_string(step.at("pid").get<uint64_t>());
        const auto tid = std::to_string(step.at("tid").get<uint64_t>());
        const auto lane = source.cpu_nodes_on_lane(pid, tid);
        auto [frontier, inserted] = lane_ends.try_emplace({ pid, tid });
        if (inserted) {
            uint64_t end_us = 0;
            for (const auto id : lane) {
                const auto & event = graph.event_for_node(id);
                end_us = std::max(end_us, event.ts + event.dur + graph.node(id).original_cpu_gap_after);
                frontier->second.push_back(end_us);
            }
        }

        // Include the preceding gap, not only leaves overlapping the forward.
        // Restore graph order so integer budget apportionment is unchanged.
        const auto first = static_cast<size_t>(std::ranges::upper_bound(frontier->second, begin) - frontier->second.begin());
        const auto last = static_cast<size_t>(std::ranges::lower_bound(lane, end, {}, [&](size_t id) { return graph.event_for_node(id).ts; }) - lane.begin());
        const auto selected = lane.subspan(first, last - first);
        std::vector<size_t> candidates(selected.begin(), selected.end());
        std::ranges::sort(candidates);

        struct Piece {
            size_t id;
            bool gap;
            uint64_t before;
            uint64_t weight;
            uint64_t begin;
            uint64_t end;
        };
        std::vector<Piece> pieces;
        uint64_t total = 0;
        for (const auto id : candidates) {
            const auto & node = graph.node(id);
            if (!node.active || !node.is_cpu || reserved.contains(node.id)) continue;
            const auto & event = graph.event_for_node(node.id);
            if (event.pid != pid || event.tid != tid) continue;
            const auto finish = event.ts + event.dur;
            const auto lo = std::max(begin, finish), hi = std::min(end, finish + node.original_cpu_gap_after);
            if (hi > lo) {
                if (node.cpu_gap_after != node.original_cpu_gap_after || graph.scope_gap_duration(node.id))
                    throw std::runtime_error("CPU budget requires unchanged unscoped gaps");
                pieces.push_back({ node.id, true, node.cpu_gap_after, hi - lo, lo, hi });
                total += hi - lo;
            }
            if (event.ts >= begin && finish <= end && event.dur && !event.name.starts_with("AscendCL@aclrtSynchronize")) {
                if (node.duration != event.dur) throw std::runtime_error("ordinary node already remodeled");
                pieces.push_back({ node.id, false, node.duration, node.duration, event.ts, finish });
                total += node.duration;
            }
        }
        if (!total || reduction > total) throw std::runtime_error("CPU budget exceeds eligible service");
        uint64_t cumulative = 0, assigned = 0;
        for (const auto & piece : pieces) {
            cumulative += piece.weight;
            const auto projected = static_cast<uint64_t>((static_cast<__uint128_t>(cumulative) * reduction) / total);
            if (!owners.emplace(std::pair{ piece.id, piece.gap }, true).second) throw std::runtime_error("overlapping phase CPU ownership");
            changes.push_back({ piece.id, piece.gap, piece.before, piece.before - (projected - assigned), piece.begin, piece.end });
            assigned = projected;
        }
        if (assigned != reduction) throw std::runtime_error("Forward redistribution changed total CPU budget");
    }
    return changes;
}

void attach_forward_cpu_service(core::DagGraph & graph, const std::vector<ForwardCpuAdjustment> & changes) {
    std::map<core::CpuServiceCost::Lane, std::vector<core::CpuServiceCost::Span>> spans;
    for (const auto & change : changes) {
        const auto reduction = change.before - change.after;
        if (!reduction) continue;
        const auto width = change.end_us - change.begin_us;
        if (reduction > width) throw std::runtime_error("Forward service reduction exceeds its measured slice");
        const auto & event = graph.event_for_node(change.node);
        spans[{ event.pid, event.tid }].push_back({ change.begin_us, change.end_us, width - reduction });
    }
    for (auto & [lane, intervals] : spans) graph.cpu_service_cost().add_spans(lane, std::move(intervals));

#ifdef DEBUG
    // Explicit migration diagnostics compare the complete node/gap view.
    // Production allocation already checks geometry, unique ownership, budget
    // preservation and nonoverlapping spans; it need not query every value again.
    for (const auto & change : changes) {
        const auto service = change.gap ? graph.cpu_service_gap_duration(change.node) : graph.cpu_service_node_duration(change.node);
        if (service != change.after) throw std::runtime_error("Forward service migration changed an allocated cost");
    }
#endif
}
} // namespace markov::trace_graph::modules::hicache
