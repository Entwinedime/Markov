#include "markov/trace_graph/modules/hicache/runtime/phase_calls.hpp"
#include "markov/trace_graph/adapters/frameworks/sglang/attention_program.hpp"
#include "markov/trace_graph/adapters/frameworks/sglang/source_calls.hpp"
#include "markov/trace_graph/source_dag/index.hpp"

#include "markov/trace_graph/modules/hicache/execution_boundaries.hpp"
#include "markov/trace_graph/modules/hicache/runtime/host_removal.hpp"
#include "markov/trace_graph/modules/hicache/runtime/tensor_program.hpp"
#include "markov/trace_graph/structure/missing_work.hpp"

#include <algorithm>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <utility>
#include <vector>

namespace markov::trace_graph::modules::hicache::runtime {
namespace {

void describe_phase_call(HiCacheHostExpansion & plan, const structure::hicache::PhaseCallPlan & work) {
    core::TraceArgMap attributes{
        {    "phase_tensor_request_id",                              work.request_id },
        { "phase_tensor_logical_input",           std::to_string(work.logical_input) },
        {         "phase_tensor_phase", work.decode_iteration ? "decode" : "prefill" },
        {     "phase_tensor_call_kind",                               work.call_kind }
    };
    if (work.decode_iteration) attributes["phase_tensor_iteration"] = std::to_string(*work.decode_iteration);
    if (work.layer_index) attributes["phase_tensor_layer"] = std::to_string(*work.layer_index);
    const auto description = std::make_shared<const core::TraceArgMap>(std::move(attributes));
    for (auto & node : plan.nodes) node.work.shared_attrs = description;
}

void inherit_collective_completions(const core::DagGraph & graph, const HiCacheHostExpansion & original, HiCacheHostExpansion & generated) {
    for (const auto & wait : original.collective_completions) {
        // Only an input dependency at the first device on a stream can move
        // to the generated program's first operation. An internal collective
        // consumer needs its own semantic mapping; never move it to entry.
        const auto old_stream = std::ranges::find(original.streams, wait.consumer, &HiCacheHostExpansion::Stream::first);
        if (old_stream == original.streams.end()) throw std::runtime_error("Phase replacement needs a semantic mapping for an internal collective consumer");
        const auto lane = graph.node(old_stream->source_node).lane_id;
        const auto new_stream = std::ranges::find_if(generated.streams, [&](const auto & stream) { return graph.node(stream.source_node).lane_id == lane; });
        if (new_stream == generated.streams.end()) throw std::runtime_error("Generated phase lacks the collective result consumer stream");
        generated.collective_completions.push_back({ wait.source_node, new_stream->first });
    }
}

} // namespace

void HiCachePhaseCalls::bind(core::DagGraph & graph, const core::DagGraph & original, std::span<const adapters::sglang::AttentionCall> calls,
                             std::span<const adapters::sglang::AttentionMetadataCall> metadata, std::span<const adapters::sglang::DenseCall> dense,
                             uint64_t begin_us, uint64_t end_us) {
    struct Boundary {
        size_t observation;
        const adapters::sglang::AscendAttentionConditions * conditions;
    };
    std::vector<Boundary> selected;
    std::vector<core::TraceEvent> points;
    const auto select = [&](size_t observation, const adapters::sglang::AscendAttentionConditions * conditions) {
        const auto & event = original.runtime_observations().at(observation);
        // Call ownership was resolved against admitted source phases. Keep
        // crossing calls and their explicitly retained causal tail together;
        // truncating either leaves source consumers of replaced operations.
        if (!is_admitted_execution_call(event, begin_us, end_us)) return;

        selected.push_back({ observation, conditions });
        for (const auto at : { event.ts, event.ts + event.dur }) {
            core::TraceEvent point;
            point.name = "attention.replacement";
            point.pid = event.pid;
            point.tid = event.tid;
            point.ts = at;
            points.push_back(std::move(point));
        }
    };
    for (const auto & call : calls) select(call.region.work.observation, &call.conditions);
    for (const auto & call : metadata) select(call.region.work.observation, nullptr);
    for (const auto & call : dense) select(call.region.work.observation, nullptr);
    if (selected.empty()) return;

    const auto boundaries = bind_hicache_control_points(graph, points);
    const source_dag::SourceDagIndex source(graph);
    const auto queues = simulation::detail::discover_cpu_task_queues(graph);
    std::vector<HiCacheHostTemplate> hosts;
    std::vector<Call> replacements;
    std::map<std::pair<size_t, size_t>, std::optional<HostDeviceResources>> observed_resources;
    hosts.reserve(selected.size());
    replacements.reserve(selected.size());
    for (size_t i = 0; i < selected.size(); ++i) {
        const auto observation = selected[i].observation;
        const auto & event = original.runtime_observations().at(observation);
        if (!boundaries[2 * i] || !boundaries[2 * i + 1]) throw std::runtime_error("Phase replacement requires exact source call entry/exit boundaries");

        hosts.push_back(observe_host_template(source, event.pid, event.tid, event.ts, event.ts + event.dur));
        auto checks = layers_.nested_calls(graph, event.pid, event.tid, event.ts, event.ts + event.dur);
        const auto * conditions = selected[i].conditions;
        if (!conditions && !checks.empty()) throw std::runtime_error("Metadata or dense work cannot own per-layer HiCache checks");
        if (conditions && conditions->layer_counter_registered) {
            if (*conditions->layer_counter_registered) {
                if (checks.size() != 2 || checks[0].batch_start_ns != checks[1].batch_start_ns || checks[0].layer != conditions->cache_layer_index.value()
                    || checks[1].layer != checks[0].layer)
                    throw std::runtime_error("Ordinary MHA attention requires its two corresponding layer checks");
            }
            else if (!checks.empty()) { throw std::runtime_error("Attention layer-counter metadata contradicts its observed checks"); }
        }
        std::vector<size_t> callbacks;
        for (const auto & check : checks) callbacks.push_back(check.anchor);
        auto retained = prepare_host_expansion(source,
                                               queues,
                                               hosts.back(),
                                               "retained phase work: ",
                                               { .callback_points = callbacks, .retain_collective_completions = true, .compact = compact_programs_ });
        std::optional<HostDeviceResources> resources;
        if (const auto compute = resources_.find({ event.pid, event.tid }); compute != resources_.end()) {
            const auto key = std::pair{ graph.node(*boundaries[2 * i]).lane_id, graph.node(compute->second).lane_id };
            const auto [at, inserted] = observed_resources.try_emplace(key);
            if (inserted) at->second = observe_host_device_resources(source, queues, *boundaries[2 * i], compute->second);
            resources = at->second;
        }
        replacements.push_back(
            { observation, *boundaries[2 * i], *boundaries[2 * i + 1], event.pid, event.tid, event.ts, std::move(retained), std::move(checks), {}, resources });
    }

    std::vector<HiCacheHostRegion> regions;
    std::vector<size_t> collective_handoffs;
    for (size_t i = 0; i < replacements.size(); ++i) regions.push_back({ replacements[i].entry, replacements[i].exit, &hosts[i] });
    for (const auto & call : replacements) {
        for (const auto & wait : call.retained.collective_completions) {
            for (const auto & [source_node, local] : call.retained.source_nodes) {
                if (local != wait.consumer) continue;
                for (const auto edge_id : source.incoming_edge_ids(source_node)) {
                    const auto & edge = graph.edge(edge_id);
                    if (edge.active && edge.kind == core::DagEdgeKind::HCCL && edge.src == wait.source_node) collective_handoffs.push_back(edge_id);
                }
            }
        }
    }
    (void)core::apply_dag_mutation_plan(graph, plan_host_removal(source, queues, regions, collective_handoffs));

    for (auto & call : replacements) {
        for (const auto & [original_node, local] : call.retained.source_nodes) owned_.insert(original_node);
        if (!calls_.emplace(call.entry, std::move(call)).second) throw std::runtime_error("Phase calls share a replacement entry");
    }
}

void HiCachePhaseCalls::rebind_retained_resources(const source_dag::SourceDagIndex & source, const std::map<size_t, size_t> & workers) {
    std::vector<HiCacheHostExpansion *> retained;
    for (auto & [entry, call] : calls_) {
        retained.push_back(&call.retained);
        auto resources = call.retained.resource_nodes();
        if (call.resources) {
            call.resources->worker = workers.at(source.graph().node(call.resources->worker).lane_id);
            resources.push_back(call.resources->compute);
        }
        for (const auto resource : resources) {
            const auto lane = source.graph().node(resource).lane_id;
            if (!call.positions.contains(lane))
                call.positions.emplace(lane, observe_write_stream_position(source, resource, call.pid, call.tid, call.begin_us));
        }
    }
    rebind_host_worker_queues(source.graph(), workers, retained);
}

void HiCachePhaseCalls::set_work(structure::hicache::PhaseCallPlan work) { work_.insert_or_assign(work.source_observation, std::move(work)); }

void HiCachePhaseCalls::advance(core::DagGraph & graph, size_t node, const HiCacheLayerCalls::Consumer & consumer, simulation::FutureDag & future) {
    const auto found = calls_.find(node);
    if (found == calls_.end()) return;
    auto & call = found->second;
    const auto & work = work_.at(call.observation);
    using Action = structure::hicache::PhaseCallPlan::Action;
    if (work.action == Action::Unavailable)
        throw structure::MissingWork("Target phase work unavailable",
                                     {
                                         { "source_observation", work.source_observation },
                                         {             "reason",             work.reason },
                                         {         "request_id",         work.request_id },
                                         {      "logical_input",      work.logical_input }
        });
    HiCacheHostSequence sequence(call.positions, insertions_, future, node);
    auto caller_lane = call.retained.nodes.front().work.lane_key;
    size_t host_return;
    uint64_t outside_us = 0;
    if (work.action == Action::RetainCompute) {
        if (retained_source_nodes_) describe_phase_call(call.retained, work);
        const auto lanes = call.retained.resource_lanes(graph);
        const auto expanded = sequence.append(std::move(call.retained), lanes);
        outside_us = layers_.rebind_copied_calls(graph, call.layers, expanded);
        if (retained_source_nodes_)
            for (const auto & [original, local] : expanded.source_nodes) retained_source_nodes_->emplace_back(original, expanded.nodes.at(local));
        host_return = expanded.host_return;
    }
    else {
        if (!call.resources) throw std::runtime_error("Target phase work lacks a proven compute stream and worker resource");
        const std::vector<size_t> no_load;
        const auto & records = call.layers.empty() ? no_load : consumer(call.pid, call.layers.front().batch_start_ns);
        auto generated = generate_tensor_program(graph,
                                                 node,
                                                 call.resources->worker,
                                                 call.resources->compute,
                                                 work.operations,
                                                 "phase_tensor",
                                                 !records.empty(),
                                                 retained_source_nodes_ != nullptr);
        inherit_collective_completions(graph, call.retained, generated.plan);
        bind_host_operations(generated.plan, generated.operations, provider_, "tensor_independent_model");
        if (retained_source_nodes_) describe_phase_call(generated.plan, work);

        size_t check = 0;
        for (size_t i = 0; i < work.operations.size(); ++i) {
            if (work.operations[i].execution != structure::TensorExecutionKind::LayerReady) continue;
            const auto & observed = call.layers.at(check++);
            // In-call preparation/return scopes now own this CPU interval.
            // Retain only the part outside the replaced attention envelope.
            outside_us += observed.following_gap_outside_us;
        }
        if (check != call.layers.size()) throw std::runtime_error("Generated attention did not take ownership of every nested layer check");

        std::vector<size_t> completions;
        for (const auto layer : generated.event_layers) completions.push_back(records.at(layer));
        const auto lanes = generated.plan.resource_lanes(graph);
        const auto expanded = sequence.append(std::move(generated.plan), lanes, completions);
        layers_.handoff_replaced_calls(graph, call.layers);
        host_return = expanded.host_return;
    }
    const auto returned = future.append({ .name = "after target phase call", .lane_key = std::move(caller_lane), .cpu_gap_after = outside_us });
    future.depend(host_return, returned);
    future.depend(returned, call.exit, core::DagEdgeKind::Sequential);

    // Expanded work now lives in the DAG. Release this consumer's source
    // template and work copy; ownership and total prepared counts stay intact.
    work_.erase(call.observation);
    calls_.erase(found);
    ++executed_;
}

} // namespace markov::trace_graph::modules::hicache::runtime
