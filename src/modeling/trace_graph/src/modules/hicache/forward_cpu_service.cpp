#include "markov/trace_graph/modules/hicache/forward_cpu_service.hpp"
#include "markov/trace_graph/modules/hicache/layer_waits.hpp"
#include "markov/trace_graph/modules/hicache/runtime/preparation.hpp"
#include <nlohmann/json.hpp>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <tuple>

namespace markov::trace_graph::modules::hicache {

std::vector<ForwardCpuAdjustment> plan_forward_cpu_service(
    const core::DagGraph& graph, const nlohmann::json& steps, const nlohmann::json& comparison) {
    std::set<size_t> reserved;
    const patch::HiCacheSourceDagIndex source(graph);
    const auto waits = observe_hicache_layer_waits(source);
    if (waits.status != "ready") throw std::runtime_error("CPU normalization needs complete layer ownership");
    reserved.insert(waits.cpu_node_ids.begin(), waits.cpu_node_ids.end());
    for (const auto& call : waits.calls) {
        for (const auto id : {call.before, call.after, call.submission, call.worker})
            if (id) reserved.insert(*id);
        for (const auto& slice : call.cpu.owned_gap_slices) reserved.insert(slice.owner_node_id);
    }
    for (const auto& call : runtime::observe_allocator_preparations(graph).calls)
        if (call.submit_gap_node) reserved.insert(*call.submit_gap_node);
    using Key = std::tuple<std::string, int, std::string, size_t>;
    std::map<Key, double> reductions;
    std::map<Key, int64_t> layer_reductions;
    for (const auto& row : comparison.at("rows")) {
        Key key{row.at("request_id"), row.at("rank"), row.at("phase"), row.value("forward_ordinal", size_t{0})};
        if (!reductions.emplace(key, row.at("delta").at("outside_sync_cpu_us")).second)
            throw std::runtime_error("duplicate CPU budget identity");
        layer_reductions.emplace(key, row.value("layer_reduction_us", int64_t{0}));
    }
    std::vector<ForwardCpuAdjustment> changes;
    std::map<std::pair<size_t, bool>, bool> owners;
    for (const auto& step : steps.at("rows")) {
        const std::string name = step.at("name");
        const Key key{step.at("request_id"), step.at("identity").at("tp_rank"), name.substr(0, name.find(' ')),
                      step.value("forward_ordinal", size_t{0})};
        const auto delta = reductions.at(key);
        if (delta < 0 || !std::isfinite(delta)) throw std::runtime_error("Forward CPU allocation requires a nonnegative measured reduction");
        const auto ordinary = static_cast<__int128>(std::llround(delta)) - layer_reductions.at(key);
        if (ordinary < 0 || ordinary > std::numeric_limits<uint64_t>::max())
            throw std::runtime_error("Layer CPU allocation leaves an invalid ordinary forward budget");
        const auto reduction = static_cast<uint64_t>(ordinary);
        const auto begin = step.at("start_ns").get<uint64_t>() / 1000;
        const auto end = step.at("end_ns").get<uint64_t>() / 1000;
        const auto pid = std::to_string(step.at("pid").get<uint64_t>());
        const auto tid = std::to_string(step.at("tid").get<uint64_t>());
        struct Piece { size_t id; bool gap; uint64_t before; uint64_t weight; uint64_t begin; uint64_t end; };
        std::vector<Piece> pieces;
        uint64_t total = 0;
        for (const auto& node : graph.nodes()) {
            if (!node.active || !node.is_cpu || reserved.contains(node.id)) continue;
            const auto& event = graph.event_for_node(node.id);
            if (event.pid != pid || event.tid != tid) continue;
            const auto finish = event.ts + event.dur;
            const auto lo = std::max(begin, finish), hi = std::min(end, finish + node.original_cpu_gap_after);
            if (hi > lo) {
                if (node.cpu_gap_after != node.original_cpu_gap_after || graph.scope_gap_duration(node.id))
                    throw std::runtime_error("CPU budget requires unchanged unscoped gaps");
                pieces.push_back({node.id, true, node.cpu_gap_after, hi-lo, lo, hi}); total += hi-lo;
            }
            if (event.ts >= begin && finish <= end && event.dur &&
                !event.name.starts_with("AscendCL@aclrtSynchronize")) {
                if (node.duration != event.dur) throw std::runtime_error("ordinary node already remodeled");
                pieces.push_back({node.id, false, node.duration, node.duration, event.ts, finish}); total += node.duration;
            }
        }
        if (!total || reduction > total) throw std::runtime_error("CPU budget exceeds eligible service");
        uint64_t cumulative = 0, assigned = 0;
        for (const auto& piece : pieces) {
            cumulative += piece.weight;
            const auto projected = static_cast<uint64_t>((static_cast<__uint128_t>(cumulative) * reduction) / total);
            if (!owners.emplace(std::pair{piece.id, piece.gap}, true).second)
                throw std::runtime_error("overlapping phase CPU ownership");
            changes.push_back({piece.id, piece.gap, piece.before, piece.before-(projected-assigned),piece.begin,piece.end});
            assigned = projected;
        }
        if (assigned != reduction) throw std::runtime_error("Forward redistribution changed total CPU budget");
    }
    return changes;
}

void attach_forward_cpu_service(core::DagGraph& graph, const std::vector<ForwardCpuAdjustment>& changes) {
    for (const auto& change : changes) {
        const auto reduction = change.before-change.after;
        if (!reduction) continue;
        const auto width = change.end_us-change.begin_us;
        if (reduction > width) throw std::runtime_error("Forward service reduction exceeds its measured slice");
        const auto& event = graph.event_for_node(change.node);
        graph.cpu_service_cost().add({event.pid,event.tid},{change.begin_us,change.end_us,width-reduction});
    }
    // Before any business transformation, the coordinate representation must
    // reproduce every old whole-node/gap value, not just the total budget.
    for (const auto& change : changes) {
        const auto service = change.gap ? graph.cpu_service_gap_duration(change.node) : graph.cpu_service_node_duration(change.node);
        if (service != change.after) throw std::runtime_error("Forward service migration changed an allocated cost");
    }
}
} // namespace markov::trace_graph::modules::hicache
