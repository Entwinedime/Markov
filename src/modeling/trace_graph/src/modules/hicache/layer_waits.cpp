#include "markov/trace_graph/modules/hicache/layer_waits.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <map>

namespace markov::trace_graph::modules::hicache {
namespace {
using Json = nlohmann::json;
using core::DagEdgeKind;

uint64_t start_ns(const core::TraceEvent & event) { return event.ts * 1000 + event.ts_submicro_ns; }
uint64_t end_ns(const core::TraceEvent & event) { return start_ns(event) + event.dur * 1000 + event.dur_submicro_ns; }

void observe_call(const patch::HiCacheSourceDagIndex & source, const core::TraceEvent & batch,
                  std::span<const size_t> lane, HiCacheLayerWaitCall & call) {
    const auto & graph = source.graph();
    const auto first = std::ranges::lower_bound(lane, call.start_ns, {}, [&](size_t id) { return start_ns(graph.event_for_node(id)); });
    const auto last = std::ranges::lower_bound(first, lane.end(), call.end_ns, {}, [&](size_t id) { return start_ns(graph.event_for_node(id)); });
    if (first == lane.begin() || last == lane.end()) { call.issue = "missing_cpu_boundaries"; return; }
    call.before = *std::prev(first);
    call.after = *last;
    call.logical_input = graph.node(*call.before).gpu_id;
    if (end_ns(graph.event_for_node(*call.before)) > call.start_ns) { call.issue = "call_cuts_cpu_leaf"; return; }
    for (auto at = first; at != last; ++at) {
        if (end_ns(graph.event_for_node(*at)) > call.end_ns) { call.issue = "call_cuts_cpu_leaf"; return; }
    }
    const auto begin_us = call.start_ns / 1000, stop_us = call.end_ns / 1000;
    call.cpu = source.timing_interval_ownership(batch.pid, batch.tid, begin_us, stop_us - begin_us);
    // A sub-microsecond no-op can occupy a real gap without owning a whole
    // simulator tick. Keep its exact position; do not invent a one-us leaf.
    if (!call.enabled) {
        if (first != last) call.issue = "inactive_call_contains_cpu_work";
        else if (call.cpu.status != "ready" && call.cpu.status != "zero_duration") call.issue = "incomplete_cpu_interval";
        return;
    }
    if (call.cpu.status != "ready" || call.cpu.owned_node_ids.size() != 1) {
        call.issue = "incomplete_submission_interval"; return;
    }
    call.submission = call.cpu.owned_node_ids.front();
    if (graph.event_for_node(*call.submission).cat != "enqueue") { call.issue = "missing_enqueue"; return; }
    std::vector<size_t> workers, waits, records;
    for (const auto id : source.outgoing_edge_ids(*call.submission)) {
        const auto & edge = graph.edge(id);
        if (edge.active && edge.kind == DagEdgeKind::Correlation && graph.node(edge.dst).is_cpu
            && graph.node(edge.dst).lane_id != graph.node(*call.submission).lane_id) workers.push_back(edge.dst);
    }
    if (workers.size() != 1) { call.issue = "nonunique_worker"; return; }
    call.worker = workers.front();
    const auto & worker = graph.event_for_node(*call.worker);
    if (worker.name != "AscendCL@aclrtStreamWaitEvent") { call.issue = "worker_is_not_stream_wait"; return; }
    for (const auto id : source.outgoing_edge_ids(*call.worker)) {
        const auto & edge = graph.edge(id);
        if (edge.active && edge.kind == DagEdgeKind::Correlation && !graph.node(edge.dst).is_cpu) waits.push_back(edge.dst);
    }
    if (waits.size() != 1) { call.issue = "nonunique_device_wait"; return; }
    call.device_wait = waits.front();
    for (const auto id : source.incoming_edge_ids(*call.device_wait)) {
        const auto & edge = graph.edge(id);
        if (edge.active && edge.kind == DagEdgeKind::Sync) records.push_back(edge.src);
    }
    if (records.size() != 1) { call.issue = "nonunique_layer_record"; return; }
    call.record = records.front();
}
} // namespace

HiCacheLayerWaitObservation observe_hicache_layer_waits(const patch::HiCacheSourceDagIndex & source) {
    HiCacheLayerWaitObservation result;
    std::map<std::pair<std::string, std::string>, std::vector<size_t>> precise_lanes;
    for (const auto & batch : source.graph().runtime_observations()) {
        if (batch.name != "runtime.hicache.layer_waits") continue;
        result.status = "ready";
        if (batch.arg("wait_clock") != "profiler_ns") { ++result.issues["unaligned_call_clock"]; continue; }
        if (batch.arg("status") != "returned") { ++result.issues["incomplete_forward"]; continue; }
        const auto requests = Json::parse(batch.arg("request_ids"));
        const auto intervals = Json::parse(batch.arg("wait_intervals"));
        if (requests.size() != 1) { ++result.issues["nonsequential_batch"]; continue; }
        if (intervals.empty()) { ++result.issues["missing_call_sites"]; continue; }
        auto [lane, inserted] = precise_lanes.try_emplace({batch.pid, batch.tid});
        if (inserted) {
            const auto nodes = source.cpu_nodes_on_lane(batch.pid, batch.tid);
            lane->second.assign(nodes.begin(), nodes.end());
            // Exact call sites need fractional order; the materialized CPU
            // gap index must keep the builder's integer self-fragment order.
            std::ranges::sort(lane->second, {}, [&](size_t id) { return std::pair{start_ns(source.graph().event_for_node(id)), id}; });
        }
        const bool enabled = std::stoll(batch.arg("consumer_index")) >= 0;
        std::map<uint64_t, size_t> positions;
        uint64_t previous_end = 0;
        for (const auto & interval : intervals) {
            HiCacheLayerWaitCall call;
            call.request_id = requests.at(0).get<std::string>();
            call.phase = batch.arg("phase");
            call.layer = interval.at(0).get<uint64_t>();
            call.position = positions[call.layer]++;
            call.enabled = enabled;
            call.start_ns = interval.at(1).get<uint64_t>();
            call.end_ns = interval.at(2).get<uint64_t>();
            if (call.end_ns < call.start_ns || call.start_ns < previous_end || call.layer >= batch.arg_u64("layer_count"))
                call.issue = "invalid_call_order_or_layer";
            else observe_call(source, batch, lane->second, call);
            previous_end = call.end_ns;
            if (!call.issue.empty()) ++result.issues[call.issue];
            else if (call.enabled) {
                result.cpu_node_ids.insert(*call.submission);
                result.cpu_node_ids.insert(*call.worker);
                result.device_wait_node_ids.insert(*call.device_wait);
            }
            result.calls.push_back(std::move(call));
        }
    }
    if (!result.issues.empty()) result.status = "partial";
    return result;
}
} // namespace markov::trace_graph::modules::hicache
