#include "markov/trace_graph/modules/hicache/runtime/layer_calls.hpp"
#include "markov/trace_graph/adapters/frameworks/sglang/hicache_observations.hpp"
#include "markov/trace_graph/cost/host_operation.hpp"
#include "markov/trace_graph/modules/hicache/patch/layer_wait_patch.hpp"
#include "markov/trace_graph/structure/ascend_event.hpp"
#include <algorithm>
#include <set>
#include <stdexcept>
#include <utility>

namespace markov::trace_graph::modules::hicache::runtime {

void HiCacheLayerCalls::bind(core::DagGraph & graph, uint64_t begin, uint64_t end) {
    const adapters::sglang::HiCacheSourceIndex source(graph);
    const auto waits = observe_hicache_layer_waits(source);
    if (waits.status != "ready") throw std::runtime_error("Dynamic layer calls need complete base wait observations");
    std::vector<patch::HiCacheLayerWaitRemoval> removals;
    std::set<size_t> removed_devices;
    std::map<size_t, size_t> stream_samples;
    for (const auto & call : waits.calls) {
        // Admission belongs to the forward, not each nested wait. The input
        // graph retains the whole already-started forward across HTTP end;
        // its tail waits must also release their original event producers.
        const bool in_window = call.batch_start_ns / 1'000 >= begin && call.batch_start_ns / 1'000 <= end;
        if (!call.enabled) continue;
        if (!in_window) continue;
        removals.push_back({ &call, 0 });
        removed_devices.insert(*call.device_wait);
        stream_samples.emplace(graph.node(*call.device_wait).lane_id, *call.device_wait);
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
                   .source_start_ns = observed.start_ns,
                   .source_end_ns = observed.end_ns,
                   .source_position = observed.position,
                   .position = positions.at(*observed.device_wait),
                   .device_lane = graph.node(*observed.device_wait).lane_id };
        call.program = generate_layer_wait(graph, *observed.submission, *observed.worker, *observed.device_wait);
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
        call.following_gap_ranges = graph.observed_cpu_gap_ranges(node);
        graph.set_cpu_gap_after(node, 0);
    }
    bind_inactive(graph, waits, begin, end);

    for (const auto & [anchor, call] : calls_) {
        graph.mutable_node(anchor).observe_start = true;
        source_calls_[{ call.pid, graph.event_for_node(anchor).tid }].push_back({ anchor, 0 });
    }
    for (auto & [lane, entries] : source_calls_) {
        std::ranges::sort(entries, {}, [&](const auto & entry) { return calls_.at(entry.anchor).source_start_ns; });
        uint64_t latest_end_ns = 0;
        for (auto & entry : entries) {
            latest_end_ns = std::max(latest_end_ns, calls_.at(entry.anchor).source_end_ns);
            entry.latest_end_ns = latest_end_ns;
        }
    }
}

void HiCacheLayerCalls::rebind_retained_resources(const adapters::sglang::HiCacheSourceIndex & source, const std::map<size_t, size_t> & members) {
    const auto & graph = source.graph();
    std::vector<HiCacheHostExpansion *> templates;
    for (auto & [node, call] : calls_) {
        templates.push_back(&call.program.plan);
        const auto & event = graph.event_for_node(node);
        rebind_host_stream_position(source, call.position, call.pid, event.tid, call.source_start_ns / 1'000);
    }
    rebind_host_worker_queues(graph, members, templates);
}

std::vector<HiCacheLayerCalls::NestedCall> HiCacheLayerCalls::nested_calls(const core::DagGraph & graph, const std::string & pid, const std::string & tid,
                                                                           uint64_t begin_us, uint64_t end_us) const {
    using Order = std::pair<uint64_t, size_t>;
    std::vector<std::pair<Order, NestedCall>> selected;
    // Widen the interval arithmetic; the caller's microsecond envelope must
    // fully contain the finer nanosecond observation, with no time tolerance.
    const auto begin_ns = static_cast<unsigned __int128>(begin_us) * 1'000;
    const auto end_ns = static_cast<unsigned __int128>(end_us) * 1'000;
    const auto lane = source_calls_.find({ pid, tid });
    if (lane == source_calls_.end()) return {};

    // Prefix maxima retain earlier overlapping calls, including zero-duration
    // points at begin. Keep the exact containment checks below unchanged.
    const auto first = std::ranges::lower_bound(lane->second, begin_ns, {}, &SourceCall::latest_end_ns);
    for (auto at = first; at != lane->second.end(); ++at) {
        const auto anchor = at->anchor;
        const auto & call = calls_.at(anchor);
        if (call.source_start_ns >= end_ns) break;

        const bool overlaps = call.source_start_ns == call.source_end_ns ? call.source_start_ns >= begin_ns && call.source_start_ns < end_ns
                                                                         : call.source_end_ns > begin_ns && call.source_start_ns < end_ns;
        if (!overlaps) continue;
        if (call.source_start_ns < begin_ns || call.source_end_ns > end_ns)
            throw std::runtime_error("Attention replacement crosses a HiCache layer-check boundary");

        if (!call.following_gap_ranges) throw std::runtime_error("Nested layer check has no source coordinates for its retained gap");
        uint64_t inside_us = 0;
        for (const auto & [lo, hi] : *call.following_gap_ranges) {
            const auto inside_begin = std::max(lo, begin_us), inside_end = std::min(hi, end_us);
            if (inside_begin < inside_end) inside_us += graph.cpu_service_cost().duration({ call.pid, tid }, inside_begin, inside_end);
        }

        selected.push_back({
            Order{ call.source_start_ns, call.source_position },
            NestedCall{ anchor,
                  call.pid,
                  call.batch_start_ns,
                  call.layer,
                  inside_us, call.outside_gap_us - inside_us,
                  call.source_inactive_us,
                  call.position }
        });
    }
    std::ranges::sort(selected, {}, &std::pair<Order, NestedCall>::first);
    std::vector<NestedCall> result;
    result.reserve(selected.size());
    for (auto & [time, call] : selected) result.push_back(std::move(call));
    return result;
}

void HiCacheLayerCalls::handoff_replaced_calls(const core::DagGraph & graph, std::span<const NestedCall> calls) {
    // Validate the whole transfer before changing callback ownership.
    for (const auto & call : calls) {
        (void)calls_.at(call.anchor);
        if (graph.node(call.anchor).active) throw std::runtime_error("Layer-check handoff requires an already replaced source anchor");
    }
    for (const auto & call : calls) calls_.erase(call.anchor);
}

uint64_t HiCacheLayerCalls::rebind_copied_calls(const core::DagGraph & graph, std::span<const NestedCall> calls, const HiCacheExpandedHost & expanded) {
    std::map<size_t, Call> replacements;
    uint64_t outside_us = 0;
    const auto copied = [&](size_t source) -> std::optional<size_t> {
        const auto found = expanded.source_nodes.find(source);
        if (found == expanded.source_nodes.end()) return std::nullopt;
        return expanded.nodes.at(found->second);
    };
    const auto successor = [&](size_t source) {
        if (const auto replacement = copied(source)) return *replacement;
        if (!graph.node(source).active) throw std::runtime_error("Copied layer check has an unbound removed consumer");
        return source;
    };

    for (const auto & observed : calls) {
        if (graph.node(observed.anchor).active) throw std::runtime_error("Copied layer check requires its source anchor to be removed");
        const auto anchor = copied(observed.anchor).value();
        auto call = calls_.at(observed.anchor);
        call.outside_gap_us = observed.following_gap_inside_us;
        for (auto & node : call.successors) node = successor(node);
        for (auto & node : call.synchronizations) node = successor(node);

        // Internal neighbors were removed along with the computation and now
        // have clone IDs. Neighbors outside that region use the surviving cut
        // already resolved before simulation; never reconnect disabled nodes.
        const auto & original = observed.source_stream_position;
        if (original.before)
            if (const auto node = copied(*original.before)) call.position.before = node;
        if (original.after)
            if (const auto node = copied(*original.after)) call.position.after = node;

        if (!replacements.emplace(anchor, std::move(call)).second || calls_.contains(anchor))
            throw std::runtime_error("Copied layer checks share a callback anchor");
        outside_us += observed.following_gap_outside_us;
    }
    // Check all correspondences before moving any callback ownership.
    for (const auto & observed : calls) calls_.erase(observed.anchor);
    calls_.merge(replacements);
    return outside_us;
}

void HiCacheLayerCalls::advance(size_t node, const Consumer & consumer, simulation::FutureDag & future) {
    const auto found = calls_.find(node);
    if (found == calls_.end()) return;
    expand_call(node, found->second, consumer, future);
}

void HiCacheLayerCalls::expand_call(size_t entry, const Call & call, const Consumer & consumer, simulation::FutureDag & future) {
    const auto & records = consumer(call.pid, call.batch_start_ns);
    size_t returned;
    if (records.empty()) {
        if (call.source_inactive_us) {
            returned = future.append({ .name = "original inactive layer return", .lane_key = call.lane, .duration = *call.source_inactive_us });
        }
        else {
            structure::OperationWork work{ "hicache_io_control", "layer_wait.inactive_return", "inactive", "cpu_service", {} };
            const auto estimate = cost::estimate_host_operation(operator_provider_, work, false);
            returned = future.append(
                { .name = "target inactive layer return", .lane_key = call.lane, .duration = estimate.main_us, .cpu_gap_after = estimate.main_residual_us });
        }
        future.depend(entry, returned);
        ++inactive_;
    }
    else {
        if (!call.insertion_issue.empty())
            throw std::runtime_error("Target layer wait cannot be inserted: " + call.insertion_issue + " pid=" + call.pid
                                     + " batch_ns=" + std::to_string(call.batch_start_ns) + " layer=" + std::to_string(call.layer));
        const auto record = records.at(call.layer);
        const auto position = insertions_.position(call.device_lane, call.position);
        auto plan = call.program.plan;
        structure::OperationWork work{ "hicache_io_control", "layer_wait.submit", "ascend.stream_wait_event", "", {} };
        const auto submit =
            cost::estimate_host_operation(operator_provider_, work, true, structure::ascend_event_work(work.component, structure::DeviceEventKind::Wait));
        work.operation = "layer_wait.tail";
        const auto tail = cost::estimate_host_operation(operator_provider_, work, false);

        // The standalone and attention-contained checks query the same cost
        // scopes. Only numeric slots change here; target readiness supplied
        // above already selected the control program and completion event.
        bind_host_operation(plan, { call.program.submit, call.program.worker, call.program.device, call.program.worker_return }, submit);
        bind_host_operation(plan, { .main = call.program.tail }, tail);

        const auto expanded = expand_host(std::move(plan), std::span(&position, 1), future, entry, std::span(&record, 1));
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
