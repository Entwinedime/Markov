#include "markov/trace_graph/modules/hicache/runtime/layer_calls.hpp"
#include "markov/trace_graph/modules/hicache/patch/layer_wait_patch.hpp"
#include <algorithm>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::runtime {

void HiCacheLayerCalls::bind(core::DagGraph & graph, uint64_t begin, uint64_t end) {
    const patch::HiCacheSourceDagIndex source(graph);
    const auto waits = observe_hicache_layer_waits(source);
    if (waits.status != "ready") throw std::runtime_error("Dynamic layer calls need complete base wait observations");
    const auto queues = simulation::detail::discover_cpu_task_queues(graph);
    std::vector<patch::HiCacheLayerWaitRemoval> removals;
    std::set<size_t> removed_devices;
    std::map<size_t, size_t> stream_samples;
    std::map<std::pair<std::string, std::string>, std::vector<uint64_t>> inactive;
    decltype(inactive) preparation_inactive;
    std::set<std::pair<std::string, std::string>> workload_requests;
    for (const auto & event : graph.prelude_context_events()) {
        if (!event.has_arg_key_hint("fact") || event.arg("request_id").empty()) continue;
        if (nlohmann::json::parse(event.arg("fact")).value("class", "") == "workload_identity") workload_requests.emplace(event.pid, event.arg("request_id"));
    }
    for (const auto & batch : graph.prelude_context_events()) {
        if (batch.name != "runtime.hicache.layer_waits" || batch.arg("consumer_index") != "-1" || batch.arg("status") != "returned"
            || batch.arg("wait_clock") != "profiler_ns")
            continue;
        const auto requests = nlohmann::json::parse(batch.arg("request_ids"));
        if (requests.size() != 1 || !workload_requests.contains({ batch.pid, requests.at(0).get<std::string>() })) continue;
        const auto first = batch.ts * 1'000 + batch.ts_submicro_ns;
        const auto last = first + batch.dur * 1'000 + batch.dur_submicro_ns;
        if (last > begin * 1'000) continue;
        std::vector<uint64_t> costs;
        uint64_t previous = first;
        for (const auto & interval : nlohmann::json::parse(batch.arg("wait_intervals"))) {
            const auto start = interval.at(1).get<uint64_t>(), stop = interval.at(2).get<uint64_t>();
            if (interval.at(0).get<uint64_t>() >= batch.arg_u64("layer_count") || start < previous || stop < start || stop > last)
                throw std::runtime_error("Invalid preparation layer-call interval");
            costs.push_back(stop - start);
            previous = stop;
        }
        auto & samples = preparation_inactive[{ batch.pid, batch.arg("phase") }];
        samples.insert(samples.end(), costs.begin(), costs.end());
    }
    for (const auto & call : waits.calls) {
        const auto & anchor = graph.event_for_node(*call.before);
        const bool in_window = call.start_ns / 1'000 >= begin && call.end_ns / 1'000 <= end;
        if (!call.enabled) {
            if (in_window) inactive[{ anchor.pid, call.phase }].push_back(call.end_ns - call.start_ns);
            continue;
        }
        if (!in_window) continue;
        removals.push_back({ &call, 0 });
        removed_devices.insert(*call.device_wait);
        stream_samples.emplace(graph.node(*call.device_wait).lane_id, *call.device_wait);
    }
    // Preparation belongs to the same base capture, but never to its scored
    // execution window. Use it only for a branch absent from formal requests.
    for (auto & [key, values] : preparation_inactive)
        if (!values.empty() && !inactive.contains(key)) inactive.emplace(key, std::move(values));
    for (auto & [key, values] : inactive) {
        std::ranges::sort(values);
        const auto middle = values.size() / 2;
        inactive_ns_[key] = values.size() % 2 ? values[middle] : values[middle - 1] + (values[middle] - values[middle - 1]) / 2;
    }
    std::map<size_t, HiCacheWriteStreamPosition> positions;
    for (const auto & [lane, sample] : stream_samples) {
        const auto & order = source.device_stream_order(sample).nodes;
        std::optional<size_t> previous;
        for (const auto node : order) {
            if (removed_devices.contains(node)) positions[node].before = previous;
            else previous = node;
        }
        std::optional<size_t> next;
        for (auto node = order.rbegin(); node != order.rend(); ++node) {
            if (removed_devices.contains(*node)) positions[*node].after = next;
            else next = *node;
        }
    }
    for (const auto & removal : removals) {
        const auto & observed = *removal.call;
        const auto & event = graph.event_for_node(*observed.submission);
        Call call{ .pid = event.pid,
                   .phase = observed.phase,
                   .lane = std::string(graph.node_lane_key(*observed.submission)),
                   .batch_start_ns = observed.batch_start_ns,
                   .layer = observed.layer,
                   .position = positions.at(*observed.device_wait),
                   .device_lane = graph.node(*observed.device_wait).lane_id };
        HiCacheHostTemplate host{ .main = observed.cpu, .worker_nodes = { *observed.worker } };
        call.plan = prepare_host_expansion(source, queues, host, "target layer wait: ");
        if (call.plan.streams.size() != 1 || call.plan.event_waits.size() != 1 || !call.plan.waits.empty())
            throw std::runtime_error("Layer call template has an unsupported device layout");
        for (const auto id : source.outgoing_edge_ids(*observed.submission)) {
            const auto & edge = graph.edge(id);
            if (!edge.active || edge.dst == *observed.worker) continue;
            if (edge.kind != core::DagEdgeKind::Sequential) throw std::runtime_error("Layer submission has an unsupported CPU successor");
            call.successors.push_back(edge.dst);
        }
        for (const auto id : source.outgoing_edge_ids(*observed.device_wait)) {
            const auto & edge = graph.edge(id);
            if (edge.active && edge.kind == core::DagEdgeKind::Sync) call.synchronizations.push_back(edge.dst);
        }
        if (!calls_.emplace(*observed.submission, std::move(call)).second) throw std::runtime_error("Layer calls share a submission");
    }
    core::DagMutationPlan removal{ .component = "layer_wait_runtime" };
    const auto audit = patch::append_hicache_layer_wait_removals(source, removals, removal);
    if (audit.status != "ready") throw std::runtime_error("Layer call removal is incomplete");
    (void)core::apply_dag_mutation_plan(graph, removal);
    for (auto & [node, call] : calls_) {
        call.outside_gap_us = graph.cpu_service_gap_duration(node);
        graph.set_cpu_gap_after(node, 0);
    }
    bind_inactive(graph, waits, begin, end);
}

void HiCacheLayerCalls::rebind_workers(const core::DagGraph & graph, const std::map<size_t, size_t> & members) {
    std::vector<HiCacheHostExpansion *> templates;
    for (auto & [node, call] : calls_) templates.push_back(&call.plan);
    rebind_host_worker_queues(graph, members, templates);
}

void HiCacheLayerCalls::advance(size_t node, const Consumer & consumer, simulation::FutureDag & future) {
    const auto found = calls_.find(node);
    if (found == calls_.end()) return;
    const auto & call = found->second;
    const auto & records = consumer(call.pid, call.batch_start_ns);
    size_t returned;
    if (records.empty()) {
        if (call.source_inactive_us) {
            returned = future.append({ .name = "original inactive layer return", .lane_key = call.lane, .duration = *call.source_inactive_us });
        }
        else {
            const auto key = std::pair{ call.pid, call.phase };
            const auto sample = inactive_ns_.find(key);
            if (sample == inactive_ns_.end()) throw std::runtime_error("Base lacks an inactive layer-call return sample");
            auto & remainder = remainder_ns_[key];
            const auto ns = remainder + sample->second;
            returned = future.append({ .name = "inactive layer return", .lane_key = call.lane, .duration = ns / 1'000 });
            remainder = ns % 1'000;
        }
        ++inactive_;
    }
    else {
        if (!call.insertion_issue.empty())
            throw std::runtime_error("Target layer wait cannot be inserted: " + call.insertion_issue + " pid=" + call.pid
                                     + " batch_ns=" + std::to_string(call.batch_start_ns) + " layer=" + std::to_string(call.layer));
        const auto record = records.at(call.layer);
        const auto position = insertions_.position(call.device_lane, call.position);
        const auto expanded = expand_host(call.plan, std::span(&position, 1), future, std::nullopt, std::span(&record, 1));
        returned = expanded.host_return;
        insertions_.advance(call.device_lane, call.position, expanded.stream_tails.front());
        for (const auto successor : call.synchronizations) future.depend(expanded.stream_tails.front(), successor);
        ++active_;
    }
    const auto continuation = future.append({ .name = "after layer call", .lane_key = call.lane, .cpu_gap_after = call.outside_gap_us });
    future.depend(returned, continuation);
    for (const auto successor : call.successors) future.depend(continuation, successor, core::DagEdgeKind::Sequential);
}

} // namespace markov::trace_graph::modules::hicache::runtime
