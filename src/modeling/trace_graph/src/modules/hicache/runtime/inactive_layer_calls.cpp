#include "markov/trace_graph/modules/hicache/runtime/layer_calls.hpp"
#include "markov/trace_graph/modules/hicache/patch/layer_wait_insertion.hpp"
#include "markov/trace_graph/modules/hicache/runtime/load_execution.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::runtime {

void HiCacheLayerCalls::bind_inactive(core::DagGraph & graph, const HiCacheLayerWaitObservation & waits, uint64_t begin, uint64_t end) {
    using Key = std::pair<std::string, std::string>;
    std::map<Key, std::vector<const Call *>> donors;
    for (const auto & [node, call] : calls_) donors[{call.pid, call.phase}].push_back(&call);
    const auto work = [](const Call * call) {
        uint64_t total = 0;
        for (const auto & node : call->plan.nodes) total += node.work.duration + node.work.cpu_gap_after;
        return total;
    };
    for (auto & [key, samples] : donors) std::ranges::stable_sort(samples, {}, work);
    std::vector<const HiCacheLayerWaitCall *> inactive;
    std::map<std::pair<size_t, size_t>, std::vector<const HiCacheLayerWaitCall *>> groups;
    for (const auto & call : waits.calls) {
        if (call.enabled || call.start_ns / 1000 < begin || call.end_ns / 1000 > end) continue;
        inactive.push_back(&call);
        groups[{*call.before, *call.after}].push_back(&call);
    }
    const patch::HiCacheSourceDagIndex source(graph);
    const auto positions = patch::observe_inactive_layer_positions(source, waits, inactive);
    struct Cost { LoadIndexOperationCost submit; uint64_t after, after_residual; };
    std::map<std::pair<int, std::string>, Cost> costs;
    if (!calibration_.empty()) {
        std::ifstream file(calibration_);
        if (!file) throw std::runtime_error("Cannot read independent layer wait costs");
        const auto doc = nlohmann::json::parse(file);
        if (doc.at("role") != "fixed_calibration" || doc.at("operation") != "layer_wait"
            || doc.at("source_manifest").get<std::string>().empty())
            throw std::runtime_error("Layer wait needs independent operation costs");
        const auto duration = [](const nlohmann::json & value) {
            const auto cost = value.get<double>();
            if (!std::isfinite(cost) || cost < 0 || cost >= std::ldexp(1.0, 64)) throw std::invalid_argument("Invalid layer wait cost");
            return static_cast<uint64_t>(std::floor(cost + 0.5));
        };
        for (const auto & row : doc.at("rows")) {
            const auto key = std::pair{row.at("rank").get<int>(), row.at("phase").get<std::string>()};
            if (!costs.emplace(key, Cost{{duration(row.at("main_us")), duration(row.at("main_residual_us")),
                duration(row.at("worker_us")), duration(row.at("dispatch_us")), duration(row.at("device_us"))},
                duration(row.at("after_us")), duration(row.at("after_residual_us"))}).second)
                throw std::runtime_error("Duplicate layer wait cost rank/phase");
        }
    }
    const auto queues = simulation::detail::discover_cpu_task_queues(graph);
    std::map<std::pair<size_t, size_t>, std::optional<LoadIndexResources>> resources;
    for (auto & [boundary, observed] : groups) {
        std::ranges::sort(observed, {}, &HiCacheLayerWaitCall::start_ns);
        const auto [before, after] = boundary;
        const auto event = graph.event_for_node(before);
        const auto gap_begin = event.ts + event.dur;
        const auto gap_end = graph.event_for_node(after).ts;
        std::vector<size_t> edges;
        for (const auto id : source.outgoing_edge_ids(before))
            if (graph.edge(id).active && graph.edge(id).kind == core::DagEdgeKind::Sequential) edges.push_back(id);
        if (edges.size() != 1 || graph.edge(edges.front()).dst != after || gap_end < gap_begin
            || graph.node(before).cpu_gap_after != gap_end - gap_begin)
            throw std::runtime_error("Inactive layer calls do not occupy one intact CPU gap");
        const auto lane = std::string(graph.node_lane_key(before));
        const auto service = [&](uint64_t lo,uint64_t hi) {
            return graph.cpu_service_cost().duration({event.pid,event.tid},lo,hi);
        };
        std::vector<size_t> gates;
        uint64_t previous_end = gap_begin * 1000;
        for (size_t i = 0; i < observed.size(); ++i) {
            const auto & call = *observed[i];
            if (call.start_ns < previous_end || call.end_ns > gap_end * 1000)
                throw std::runtime_error("Inactive layer calls overlap or escape their CPU gap");
            previous_end = call.end_ns;
            const auto samples = donors.find({event.pid, call.phase});
            const auto & position = positions.at(&call);
            if (position.before.size() > 1) throw std::runtime_error("Inactive layer call has multiple stream predecessors");
            const auto gate = graph.add_synthetic_node({ .name = "inactive layer call entry", .category = "execution_gate",
                .lane_key = lane, .observed_point = core::DagObservedPoint{event.pid, event.tid, call.start_ns / 1000, graph.node(before).gpu_id} });
            const auto next = i + 1 < observed.size() ? observed[i + 1]->start_ns / 1000 : gap_end;
            Call target{ .pid = event.pid, .phase = call.phase, .lane = lane,
                .batch_start_ns = call.batch_start_ns, .layer = call.layer,
                .outside_gap_us = service(call.end_ns / 1000,next),
                .source_inactive_us = service(call.start_ns / 1000,call.end_ns / 1000),
                .plan = samples == donors.end() ? HiCacheHostExpansion{} : samples->second[samples->second.size() / 2]->plan,
                .position = {position.before.empty() ? std::nullopt : std::optional{position.before.front()}, position.after},
                .device_lane = position.device_lane, .successors = {}, .synchronizations = position.synchronizations,
                .insertion_issue = samples == donors.end() ? "active_call_sample_missing" : position.issue };
            if (samples == donors.end() && position.issue.empty()) {
                const auto cost = costs.find({graph.node(before).gpu_id, call.phase});
                const auto key = std::pair{graph.node(before).lane_id, position.device_lane};
                if (cost != costs.end()) {
                    if (!resources.contains(key)) resources.emplace(key,
                        observe_load_index_resources(source, queues, before, position.after));
                    if (const auto & r = resources.at(key)) {
                        target.plan = generate_layer_wait(graph, before, r->worker, r->compute,
                            cost->second.submit, cost->second.after, cost->second.after_residual);
                        target.insertion_issue.clear();
                    } else target.insertion_issue = "forward_worker_resource_missing";
                } else target.insertion_issue = "independent_layer_wait_cost_missing";
            } else if (samples == donors.end() && !position.issue.empty()) target.insertion_issue = position.issue;
            calls_.emplace(gate, std::move(target));
            gates.push_back(gate);
        }
        graph.mutable_edge(edges.front()).active = false;
        const auto prefix_end=observed.front()->start_ns / 1000;
        core::DagGraph::CpuGapRanges prefix;
        if (prefix_end>gap_begin) prefix.emplace_back(gap_begin,prefix_end);
        graph.set_cpu_gap_after(before,prefix_end-gap_begin,std::move(prefix));
        graph.add_edge(before, gates.front(), core::DagEdgeKind::Sequential);
        for (size_t i = 0; i < gates.size(); ++i) {
            const auto next = i + 1 < gates.size() ? gates[i + 1] : after;
            calls_.at(gates[i]).successors.push_back(next);
            graph.add_edge(gates[i], next, core::DagEdgeKind::Sequential);
        }
    }
}

} // namespace markov::trace_graph::modules::hicache::runtime
