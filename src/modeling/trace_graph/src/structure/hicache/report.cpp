#include "markov/trace_graph/structure/hicache/report.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/source_dag/call_regions.hpp"
#include "markov/trace_graph/source_dag/device_collectives.hpp"
#include "markov/trace_graph/source_dag/observations.hpp"
#include "markov/trace_graph/structure/hicache/allocator.hpp"
#include "markov/trace_graph/structure/phase_shape.hpp"
#include "markov/trace_graph/structure/tensor_program.hpp"
#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace markov::trace_graph::structure::hicache {
namespace model = modules::hicache::model;
using Json = nlohmann::json;
namespace {

struct PhaseOperator {
    size_t node;
    int rank;
    std::string request, phase, operation;
    uint64_t iteration;
    std::string implementation;
    std::optional<size_t> source_node;
};

Json operator_resource_groups(const core::DagGraph & graph, const std::map<size_t, std::string> & identities) {
    std::map<std::pair<int, size_t>, std::vector<std::string>> groups;
    for (const auto & [id, identity] : identities) {
        const auto & node = graph.node(id);
        groups[{ node.gpu_id, node.lane_id }].push_back(identity);
    }

    // Resource ids are local to a graph. Compare which semantic operations
    // share a lane, not the numeric stream or device labels across captures.
    Json result = Json::array();
    for (auto & [resource, members] : groups) {
        std::ranges::sort(members);
        result.push_back({
            { "operators", members }
        });
    }
    return result;
}

Json phase_precedence(const core::DagGraph & graph, const std::map<size_t, std::string> & identities) {
    // Contract unowned chains only for comparison, never in the model graph.
    // Keep shared junctions: the output is a sparse DAG, not all-pairs closure.
    std::vector<size_t> offsets(graph.node_count() + 1);
    std::vector<size_t> indegree(graph.node_count()), order;
    const auto active_edge = [&](const auto & edge) { return edge.active && graph.node(edge.src).active && graph.node(edge.dst).active; };
    for (const auto & edge : graph.edges()) {
        if (!active_edge(edge)) continue;
        ++offsets[edge.src + 1];
        ++indegree[edge.dst];
    }
    for (size_t node = 0; node < graph.node_count(); ++node) offsets[node + 1] += offsets[node];
    std::vector<size_t> destinations(offsets.back());
    {
        auto cursor = offsets;
        for (const auto & edge : graph.edges())
            if (active_edge(edge)) destinations[cursor[edge.src]++] = edge.dst;
    }
    const auto successors = [&](size_t node) { return std::span<const size_t>(destinations).subspan(offsets[node], offsets[node + 1] - offsets[node]); };

    for (const auto & node : graph.nodes())
        if (node.active && indegree[node.id] == 0) order.push_back(node.id);
    for (size_t index = 0; index < order.size(); ++index)
        for (const auto next : successors(order[index]))
            if (--indegree[next] == 0) order.push_back(next);

    // Named operations occupy the first slots. Additional slots are anonymous
    // dependency junctions, not new model operations. Contract single-successor
    // unowned chains, but retain forks instead of expanding their descendants:
    // a worker chain can otherwise create quadratic operation-pair output.
    // Reachability between named endpoints is unchanged by this contraction.
    std::map<size_t, size_t> indices;
    std::vector<std::string> names;
    for (const auto & [node, identity] : identities) {
        indices.emplace(node, names.size());
        names.push_back(identity);
    }
    std::vector<std::vector<size_t>> relations(names.size());
    const auto missing = std::numeric_limits<size_t>::max();
    auto representatives = std::move(indegree);
    std::ranges::fill(representatives, missing);
    for (auto node = order.rbegin(); node != order.rend(); ++node) {
        std::vector<size_t> children;
        for (const auto next : successors(*node)) {
            if (representatives[next] != missing) children.push_back(representatives[next]);
        }
        std::ranges::sort(children);
        children.erase(std::unique(children.begin(), children.end()), children.end());

        if (const auto named = indices.find(*node); named != indices.end()) {
            representatives[*node] = named->second;
            relations[named->second] = std::move(children);
        }
        else if (children.size() == 1) representatives[*node] = children.front();
        else if (!children.empty()) {
            representatives[*node] = relations.size();
            relations.push_back(std::move(children));
        }
    }
    return {
        { "identities", std::move(names) },
        { "successors", std::move(relations) },
        { "unprocessed_active_nodes", static_cast<size_t>(std::ranges::count_if(graph.nodes(), [](const auto & node) { return node.active; })) - order.size() }
    };
}

Json generated_work_inventory(const core::DagGraph & graph) {
    std::map<std::string, size_t> categories, operations, edge_groups;
    std::set<std::pair<int, size_t>> resources;
    size_t generated = 0, observation_fragments = 0;
    const auto generated_node = [&](size_t id) {
        const auto & node = graph.node(id);
        return node.kind == core::DagNodeKind::Synthetic && !node.observed_cpu_coordinates;
    };
    for (const auto & node : graph.nodes()) {
        if (!node.active || node.kind != core::DagNodeKind::Synthetic) continue;
        if (node.observed_cpu_coordinates) {
            ++observation_fragments;
            continue;
        }
        ++generated;
        const auto & event = graph.event_for_node(node.id);
        ++categories[event.cat];
        ++operations[event.name];
        resources.emplace(node.gpu_id, node.lane_id);
    }
    for (const auto & edge : graph.edges()) {
        if (!edge.active || !graph.node(edge.src).active || !graph.node(edge.dst).active) continue;
        const auto from = generated_node(edge.src) ? "generated" : "retained";
        const auto to = generated_node(edge.dst) ? "generated" : "retained";
        ++edge_groups[std::string(from) + "->" + to];
    }
    return {
        {                "generated_nodes",                                                                                                       generated },
        { "retained_observation_fragments",                                                                                           observation_fragments },
        {                     "categories",                                                                                                      categories },
        {                "operation_names",                                                                                                      operations },
        {            "resource_lane_count",                                                                                                resources.size() },
        {           "edge_endpoint_groups",                                                                                                     edge_groups },
        {                          "scope", "active synthetic operations and boundaries, excluding source observation fragments; not target correspondence" },
        {   "target_correspondence_proven",                                                                                                           false },
    };
}

std::vector<std::string> observed_kernel_names(const core::DagGraph & graph, const std::set<size_t> & devices) {
    std::vector<std::string> names;
    for (const auto device : devices) names.push_back(graph.event_for_node(device).name);
    // Source ids order this inventory, not cross-trace execution order.
    return names;
}

Json unassigned_device_work(const core::DagGraph & graph, const std::set<size_t> & devices, const std::set<size_t> & assigned) {
    size_t observed = 0, logical_waits = 0, other_synthetic = 0;
    std::map<std::string, size_t> names;
    for (const auto node : devices) {
        if (assigned.contains(node)) continue;
        const auto & event = graph.event_for_node(node);
        ++names[event.name];

        // A builder-created wait is not a missing measured kernel. It still
        // belongs to uncovered work until its target correspondence is proven.
        if (graph.node(node).kind == core::DagNodeKind::TraceEvent) ++observed;
        else if (event.name == "logical_event_wait" && event.cat == "dependency") ++logical_waits;
        else ++other_synthetic;
    }
    return {
        { "unassigned_call_device_nodes",          observed + logical_waits + other_synthetic                                         },
        {      "unassigned_kernel_names",                                                                             std::move(names) },
        { "unassigned_device_node_kinds",
         { { "observed_device_work", observed }, { "synthetic_logical_wait", logical_waits }, { "other_synthetic", other_synthetic } } }
    };
}

Json serial_native_sequence(const modules::hicache::patch::HiCacheSourceDagIndex & source, const std::set<size_t> & devices, bool predicted) {
    const auto chain = source_dag::observe_serial_device_chain(source, devices);
    if (!chain) return nullptr;

    const auto & graph = source.graph();
    Json sequence = Json::array();
    auto current = chain->first;
    while (true) {
        const auto & event = graph.event_for_node(current);
        if (predicted) {
            const auto label = source_dag::native_launch_label(event.arg("cost_implementation"));
            if (!label) return nullptr;
            sequence.push_back(std::string(*label));
        }
        else { sequence.push_back(event.name); }
        if (current == chain->last) break;

        // The chain proof establishes exactly one successor here. Traverse
        // actual stream edges; node ids and timestamps are not execution order.
        for (const auto id : source.outgoing_edge_ids(current)) {
            const auto & edge = graph.edge(id);
            if (edge.active && edge.kind == core::DagEdgeKind::Stream && devices.contains(edge.dst)) {
                current = edge.dst;
                break;
            }
        }
    }
    return sequence;
}

Json native_device_topology(const modules::hicache::patch::HiCacheSourceDagIndex & source, const std::set<size_t> & devices) {
    const auto & graph = source.graph();
    std::map<std::pair<int, size_t>, size_t> lanes;
    std::map<std::string, size_t> edges;
    std::vector<std::pair<size_t, size_t>> degrees;
    for (const auto node : devices) {
        const auto & value = graph.node(node);
        ++lanes[{ value.gpu_id, value.lane_id }];
        size_t incoming = 0, outgoing = 0;
        for (const auto id : source.incoming_edge_ids(node)) {
            const auto & edge = graph.edge(id);
            if (edge.active && devices.contains(edge.src)) ++incoming;
        }
        for (const auto id : source.outgoing_edge_ids(node)) {
            const auto & edge = graph.edge(id);
            if (!edge.active || !devices.contains(edge.dst)) continue;
            ++outgoing;
            ++edges[std::string(core::edge_kind_name(edge.kind))];
        }
        degrees.emplace_back(incoming, outgoing);
    }

    // Local node/lane ids are never cross-trace identities. These invariants
    // expose parallelism or missing links, but do not prove graph isomorphism
    // or native-operation identity when they happen to agree.
    std::vector<size_t> lane_sizes;
    for (const auto & [lane, count] : lanes) lane_sizes.push_back(count);
    std::ranges::sort(lane_sizes);
    std::ranges::sort(degrees);
    return {
        {          "lane_sizes", lane_sizes },
        { "internal_edge_kinds",      edges },
        {    "internal_degrees",    degrees }
    };
}

Json executed_tensor_operations(const core::DagGraph & graph, const std::string & family, const std::map<std::string, int> & ranks = {}) {
    const modules::hicache::patch::HiCacheSourceDagIndex source(graph);
    Json operations = Json::array();
    std::map<size_t, std::string> completions;
    std::map<std::string, Json> device_operations;
    std::map<std::string, std::set<size_t>> device_nodes;
    for (const auto & node : graph.nodes()) {
        if (!node.active) continue;
        const auto & event = graph.event_for_node(node.id);
        // Geometry is recorded once, on the actual emitted operation's
        // submission node. A saved/unexecuted work plan is not evidence here.
        if (event.arg(family + "_action") != "generated") continue;
        const auto execution = event.arg(family + "_execution");
        const bool tensor_device = !node.is_cpu && (execution == "async_device" || execution == "direct_blocking_device");
        const bool tensor_completion = (tensor_device && event.arg("tensor_intermediate_device") != "true")
                                       || (execution == "blocking_readback" && event.arg("tensor_completion") == "true");
        if (!tensor_completion && !tensor_device && !event.has_arg_key_hint("work_features")) continue;
        const auto index = event.arg_u64(family + "_operation_index");
        Json call_identity, metadata;
        if (family == "phase_tensor") {
            const auto iteration = event.has_arg_key_hint("phase_tensor_iteration") ? Json(event.arg_u64("phase_tensor_iteration")) : Json(nullptr);
            const auto layer = event.has_arg_key_hint("phase_tensor_layer") ? Json(event.arg_u64("phase_tensor_layer")) : Json(nullptr);
            const auto logical_input = std::stoi(event.arg("phase_tensor_logical_input"));
            call_identity = Json::array({ logical_input,
                                          event.arg("phase_tensor_request_id"),
                                          event.arg("phase_tensor_phase"),
                                          iteration,
                                          layer,
                                          event.arg("phase_tensor_call_kind") });
            metadata = {
                { "logical_input",                        logical_input },
                {    "request_id", event.arg("phase_tensor_request_id") },
                {         "phase",      event.arg("phase_tensor_phase") },
                {     "iteration",                            iteration },
                {   "layer_index",                                layer },
                {     "call_kind",  event.arg("phase_tensor_call_kind") }
            };
        }
        else {
            const auto rank = ranks.find(event.arg("allocator_pid"));
            const int logical_input = rank == ranks.end() ? -1 : rank->second;
            const auto requests = Json::parse(event.arg("allocator_requests"));
            call_identity = Json::array({ logical_input, requests, event.arg("allocator_phase") });
            metadata = {
                { "logical_input",                logical_input },
                {      "requests",                     requests },
                {         "phase", event.arg("allocator_phase") }
            };
        }
        auto identity = call_identity;
        identity.push_back(index);
        if (tensor_device) {
            device_nodes[identity.dump()].insert(node.id);
            auto & native = device_operations.try_emplace(identity.dump(), Json::array()).first->second;
            const auto label = source_dag::native_launch_label(event.arg("cost_implementation"));
            native.push_back({
                {           "operation",                       event.arg("cost_operation") },
                {      "implementation",                  event.arg("cost_implementation") },
                { "expected_task_label", label ? Json(std::string(*label)) : Json(nullptr) }
            });
        }
        if (tensor_completion) {
            completions.emplace(node.id, identity.dump());
            continue;
        }
        if (tensor_device) continue;
        metadata.update({
            { "operator_identity",                         identity.dump() },
            {     "call_identity",                    call_identity.dump() },
            {   "operation_index",                                   index },
            {         "operation",                  event.arg("operation") },
            {    "implementation",             event.arg("implementation") },
            {         "execution",                               execution },
            {          "features", Json::parse(event.arg("work_features")) },
        });
        operations.push_back(std::move(metadata));
    }
    for (auto & operation : operations) {
        const auto identity = operation.at("operator_identity").get<std::string>();
        const auto native = device_operations.find(identity);
        operation["native_operations"] = native == device_operations.end() ? Json::array() : std::move(native->second);
        operation["native_device_nodes"] = operation["native_operations"].size();
        operation["native_device_topology"] = native_device_topology(source, device_nodes[identity]);
        operation["native_serial_sequence"] = serial_native_sequence(source, device_nodes[identity], true);
    }

    auto precedence = phase_precedence(graph, completions);
    std::vector<std::string> completion_identities;
    for (const auto & [node, identity] : completions) completion_identities.push_back(identity);
    return {
        { "operators", std::move(operations) },
        { "completion_identities", completion_identities },
        { "resource_groups", operator_resource_groups(graph, completions) },
        { "precedence_identities", std::move(precedence["identities"]) },
        { "precedence_successors", std::move(precedence["successors"]) },
        { "precedence_unprocessed_nodes", precedence["unprocessed_active_nodes"] },
        { "scope", "actually emitted " + family + " work descriptors; native lowering and target correspondence are not proven" },
        { "target_correspondence_proven", false }
    };
}

/** Shared correspondence accounting for phase and allocator programs.
 * Call identities and framework-specific matching stay with their callers.
 * This records only independently corroborated work, never cost inference.
 */
struct ObservedTensorOperations {
    Json operators = Json::array();
    std::map<size_t, std::string> completions;
    std::set<size_t> call_devices, assigned_devices;
    size_t unavailable_programs = 0, unmatched_program = 0, overlapping_ownership = 0;
    std::map<std::string, std::map<std::string, size_t>> unmatched_operations;

    void append(const modules::hicache::patch::HiCacheSourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues,
                std::span<const TensorOperation> program, const std::map<size_t, size_t> & matched, const Json & call_identity) {
        const auto & graph = source.graph();
        for (size_t index = 0; index < program.size(); ++index) {
            const auto & operation = program[index];
            if (operation.execution == TensorExecutionKind::CpuService || operation.execution == TensorExecutionKind::LayerReady) continue;

            const auto found = matched.find(index);
            if (found == matched.end()) {
                ++unmatched_program;
                ++unmatched_operations[operation.work.operation]["operator_correspondence"];
                continue;
            }

            const auto region = source_dag::observe_submitted_call_region(source, queues, found->second);
            const auto readback =
                operation.execution == TensorExecutionKind::BlockingContextReadback ? source_dag::observe_synchronous_readback(source, region) : std::nullopt;
            std::set<size_t> devices;
            for (const auto node : region.submitted_nodes)
                if (!graph.node(node).is_cpu) devices.insert(node);

            if (region.caller.status != "ready" || region.caller.has_node_overlap || (devices.empty() && !readback)
                || (operation.execution == TensorExecutionKind::BlockingContextReadback && !readback)) {
                ++unmatched_program;
                ++unmatched_operations[operation.work.operation]["incomplete_submission_or_readback"];
                continue;
            }
            if (std::ranges::any_of(devices, [&](size_t node) { return assigned_devices.contains(node); })) {
                ++overlapping_ownership;
                continue;
            }

            assigned_devices.insert(devices.begin(), devices.end());
            auto identity = call_identity;
            identity.push_back(index);
            if (readback) completions.emplace(readback->completion, identity.dump());
            else if (const auto chain = source_dag::observe_serial_device_chain(source, devices)) completions.emplace(chain->last, identity.dump());

            operators.push_back({
                { "operator_identity", identity.dump() },
                { "call_identity", call_identity.dump() },
                { "operation", operation.work.operation },
                { "implementation", operation.work.implementation },
                { "features", operation.work.features },
                { "native_device_nodes", devices.size() },
                { "native_device_topology", native_device_topology(source, devices) },
                { "observed_kernel_names", observed_kernel_names(graph, devices) },
                { "native_serial_sequence", serial_native_sequence(source, devices, false) },
            });
        }
    }

    Json report(const core::DagGraph & graph) && {
        auto precedence = phase_precedence(graph, completions);
        Json result{
            { "operators", std::move(operators) },
            { "unmatched_program_operators", unmatched_program },
            { "unmatched_operations", std::move(unmatched_operations) },
            { "resource_groups", operator_resource_groups(graph, completions) },
            { "completion_identities", precedence["identities"] },
            { "precedence_identities", std::move(precedence["identities"]) },
            { "precedence_successors", std::move(precedence["successors"]) },
            { "precedence_unprocessed_nodes", precedence["unprocessed_active_nodes"] },
            { "unavailable_call_programs", unavailable_programs },
            { "overlapping_ownership", overlapping_ownership },
        };
        result.update(unassigned_device_work(graph, call_devices, assigned_devices));
        return result;
    }
};

Json observed_phase_tensor_operations(const modules::hicache::patch::HiCacheSourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues,
                                      const modules::hicache::HiCachePhaseObservationAudit & phases, std::map<size_t, std::string> & call_owners) {
    const auto & graph = source.graph();
    const auto audit = source_dag::observe_attention_calls(source, queues, phases);
    const auto metadata_audit = source_dag::observe_attention_metadata_calls(source, queues, phases);
    const auto dense_audit = source_dag::observe_dense_calls(source, queues, phases, audit);
    ObservedTensorOperations result;
    const auto append_call = [&](const source_dag::SubmittedCallRegion & work, std::span<const size_t> operator_observations,
                                 size_t phase_index, std::optional<uint64_t> decode_iteration, std::optional<uint64_t> cache_layer,
                                 const std::string & kind, const std::optional<std::vector<TensorOperation>> & program) {
        for (const auto node : work.submitted_nodes)
            if (!graph.node(node).is_cpu) result.call_devices.insert(node);
        if (!program) {
            ++result.unavailable_programs;
            return;
        }

        const auto matched = source_dag::match_tensor_operators(source, queues, operator_observations, *program);
        const auto & phase = phases.observations.at(phase_index);
        const auto iteration = decode_iteration ? Json(*decode_iteration) : Json(nullptr);
        const auto layer = cache_layer ? Json(*cache_layer) : Json(nullptr);
        const auto call_identity =
            Json::array({ phase.logical_input, phase.request_ids.front(), decode_iteration ? "decode" : "prefill", iteration, layer, kind });
        const auto call_key = call_identity.dump();
        for (const auto * nodes : { &work.caller.owned_node_ids, &work.submitted_nodes }) {
            for (const auto node : *nodes) {
                const auto [owner, added] = call_owners.emplace(node, call_key);
                if (!added && owner->second != call_key) owner->second.clear();
            }
        }

        result.append(source, queues, *program, matched, call_identity);
    };
    for (const auto & call : audit.calls)
        append_call(call.region.work, call.region.operator_observations, call.phase_index, call.decode_iteration, call.conditions.cache_layer_index,
                    "layer", ascend_attention_tensor_program(call.conditions, call.batch, call.cache_write));
    for (const auto & call : metadata_audit.calls)
        append_call(call.region.work, call.region.operator_observations, call.phase_index, call.decode_iteration, std::nullopt,
                    "metadata", ascend_attention_metadata_program(call.decode_iteration ? "decode" : "prefill", call.inputs));
    for (const auto & call : dense_audit.calls)
        append_call(call.region.work, call.region.operator_observations, call.phase_index, call.decode_iteration, call.layer_index,
                    "dense." + call.role, dense_tensor_program(call.decode_iteration ? "decode" : "prefill", call.region.implementation, call.region.shape));

    auto issues = audit.issues;
    for (const auto & [issue, count] : metadata_audit.issues) issues[issue] += count;
    for (const auto & [issue, count] : dense_audit.issues) issues[issue] += count;
    auto report = std::move(result).report(graph);
    report["source_observation_issues"] = std::move(issues);
    report["scope"] =
        "tensor work corroborated by Torch envelopes and submitted device work; CPU, layer checks and native dependency correspondence remain outside this comparison";
    return report;
}

Json collective_identity(int rank, const std::string & request, const std::string & phase, uint64_t iteration, uint64_t layer, const std::string & role,
                         const std::string & callsite, const Json & members) {
    return Json::array({ rank, request, phase, iteration, layer, role, callsite, members });
}

Json collective_structure(const core::DagGraph & graph, Json operators, const std::map<size_t, std::string> & completions,
                          const std::map<std::string, size_t> & issues) {
    auto precedence = phase_precedence(graph, completions);
    return {
        { "operators", std::move(operators) },
        { "resource_groups", operator_resource_groups(graph, completions) },
        { "precedence_identities", std::move(precedence["identities"]) },
        { "precedence_successors", std::move(precedence["successors"]) },
        { "precedence_unprocessed_nodes", precedence["unprocessed_active_nodes"] },
        { "issues", issues },
        { "scope", "ordinary collective payload and reachability between rank-local completions; excludes protocol internals and CPU/event correspondence" }
    };
}

Json observed_collective_structure(const core::DagGraph & graph, const modules::hicache::HiCachePhaseObservationAudit & phases,
                                   const source_dag::DeviceCollectiveAudit & audit) {
    auto issues = audit.issues;
    Json operators = Json::array();
    std::map<size_t, std::string> completions;
    for (const auto & call : audit.calls) {
        const auto & phase = phases.observations.at(call.position.phase_index);
        if (phase.request_ids.size() != 1 || call.communication_nodes.size() != 1) {
            ++issues["unresolved_request_or_completion"];
            continue;
        }

        const auto identity = collective_identity(phase.logical_input,
                                                  phase.request_ids.front(),
                                                  call.position.decode_iteration ? "decode" : "prefill",
                                                  call.position.decode_iteration.value_or(0),
                                                  call.layer_index,
                                                  call.role,
                                                  call.callsite,
                                                  call.members)
                                  .dump();
        const auto node = call.communication_nodes.front();
        if (!graph.node(node).active || !completions.emplace(node, identity).second) {
            ++issues["unavailable_or_shared_completion"];
            continue;
        }
        const std::string operation = call.transfer == CollectiveTaskKind::Gather ? "all_gather" : "all_reduce";
        operators.push_back({
            { "operator_identity",                                                                                     identity },
            {        "graph_node",                                                                                         node },
            {    "implementation",                                     "sglang.NpuCommunicator." + operation + ":" + call.dtype },
            {          "features", CollectiveShape{ call.rows, call.width, call.element_bytes, call.members.size() }.features() }
        });
    }
    return collective_structure(graph, std::move(operators), completions, issues);
}

Json executed_collective_structure(const core::DagGraph & graph, const std::vector<PhaseOperator> & work, std::span<const DeviceCollectivePlan> plans) {
    std::map<size_t, const OperationWork *> demands;
    for (const auto & plan : plans)
        if (plan.service) demands.emplace(plan.source_observation, &*plan.service);

    Json operators = Json::array();
    std::map<size_t, std::string> completions;
    std::map<std::string, size_t> issues;
    for (const auto & item : work) {
        const auto & event = graph.event_for_node(item.node);
        if (!graph.node(item.node).active || !event.has_arg_key_hint("collective_source_node")) continue;
        const auto demand = demands.find(event.arg_u64("collective_source_observation"));
        if (demand == demands.end()) {
            ++issues["generated_completion_without_admitted_work"];
            continue;
        }

        const auto identity = collective_identity(item.rank,
                                                  item.request,
                                                  item.phase,
                                                  item.iteration,
                                                  event.arg_u64("collective_layer"),
                                                  event.arg("collective_role"),
                                                  event.arg("collective_callsite"),
                                                  Json::parse(event.arg("collective_members")))
                                  .dump();
        if (!completions.emplace(item.node, identity).second) {
            ++issues["shared_completion"];
            continue;
        }
        operators.push_back({
            { "operator_identity",                       identity },
            {        "graph_node",                      item.node },
            {    "implementation", demand->second->implementation },
            {          "features",       demand->second->features }
        });
    }
    return collective_structure(graph, std::move(operators), completions, issues);
}

Json operator_structure(const core::DagGraph & graph, std::vector<PhaseOperator> work) {
    // Generic worker labels such as Node@launch serve unrelated queue tasks.
    // Qualify them by the source correlation, not by their global occurrence.
    // Keep inactive source records: retained work may have been copied to a
    // synthetic node without copying the original trace's correlation fields.
    using SubmissionIdentity = std::tuple<int, std::string, std::string>;
    std::map<SubmissionIdentity, std::vector<size_t>> submissions;
    for (const auto & node : graph.nodes()) {
        if (node.kind != core::DagNodeKind::TraceEvent) continue;
        const auto & event = graph.event_for_node(node.id);
        if (event.cat == "enqueue" && !event.arg("correlation_id").empty())
            submissions[{ node.gpu_id, event.pid, event.arg("correlation_id") }].push_back(node.id);
    }

    // Occurrences only disambiguate repeated operators within the same
    // request/round. They are not a claim of whole-graph semantic equivalence.
    std::ranges::sort(work, [&](const auto & left, const auto & right) {
        const auto & a = graph.event_for_node(left.node);
        const auto & b = graph.event_for_node(right.node);
        return std::pair{ a.ts, a.index } < std::pair{ b.ts, b.index };
    });
    Json operators = Json::array(), dependencies = Json::array();
    std::map<std::string, size_t> occurrences;
    std::map<size_t, std::string> identities;
    size_t repeated_ownership = 0, inactive_work = 0, crossing_edges = 0, unowned_edges = 0;
    for (const auto & item : work) {
        const auto & node = graph.node(item.node);
        if (!node.active) {
            ++inactive_work;
            continue;
        }
        const auto & implementation = item.implementation.empty() ? graph.event_for_node(item.node).name : item.implementation;
        Json submission = nullptr;
        if (implementation == "Node@launch") {
            const auto original = item.source_node.value_or(item.node);
            const auto & event = graph.event_for_node(original);
            const auto found = submissions.find({ graph.node(original).gpu_id, event.pid, event.arg("correlation_id") });
            if (found != submissions.end() && found->second.size() == 1) submission = graph.event_for_node(found->second.front()).name;
        }
        const auto scope = node.is_cpu ? "cpu_submission" : "device_operation";
        const auto group = Json::array({ item.rank, item.request, item.phase, item.iteration, item.operation, implementation, scope, submission });
        const auto occurrence = occurrences[group.dump()]++;
        auto identity = group;
        identity.push_back(occurrence);
        const auto key = identity.dump();
        if (!identities.emplace(item.node, key).second) {
            ++repeated_ownership;
            continue;
        }
        operators.push_back({
            {         "operator_identity",                                                       key                                          },
            {                "graph_node",                                                                                           item.node },
            {             "logical_input",                                                                                           item.rank },
            {                "request_id",                                                                                        item.request },
            {                     "phase",                                                                                          item.phase },
            {                 "iteration",                                                                                      item.iteration },
            {                 "operation",                                                                                      item.operation },
            {            "implementation",                                                                                      implementation },
            { "submission_implementation",                                                                                          submission },
            {             "resource_kind",                                                                                               scope },
            {                "occurrence",                                                                                          occurrence },
            {            "representation",
             graph.event_for_node(item.node).has_arg_key_hint("collective_source_node") ? "logical_collective_completion" : "native_operation" },
        });
    }
    for (const auto & edge : graph.edges()) {
        if (!edge.active || !graph.node(edge.src).active || !graph.node(edge.dst).active) continue;
        const auto from = identities.find(edge.src), to = identities.find(edge.dst);
        if (from != identities.end() && to != identities.end()) {
            dependencies.push_back({
                { "from",                    from->second },
                {   "to",                      to->second },
                { "kind", core::edge_kind_name(edge.kind) }
            });
        }
        else if (from != identities.end() || to != identities.end()) ++crossing_edges;
        else ++unowned_edges;
    }
    auto precedence = phase_precedence(graph, identities);
    return {
        { "operators", std::move(operators) },
        { "dependencies", std::move(dependencies) },
        { "resource_groups", operator_resource_groups(graph, identities) },
        { "precedence_identities", std::move(precedence["identities"]) },
        { "precedence_successors", std::move(precedence["successors"]) },
        { "precedence_unprocessed_nodes", precedence["unprocessed_active_nodes"] },
        { "scope", "single-request phase operators; direct edges and reachability through anonymous dependency junctions" },
        { "correspondence", "rank, request, phase, round, implementation and within-family observed occurrence" },
        { "repeated_ownership", repeated_ownership },
        { "inactive_work", inactive_work },
        { "uncompared_incident_edges", crossing_edges },
        { "unassigned_active_nodes",
         static_cast<size_t>(std::ranges::count_if(graph.nodes(), [](const auto & node) { return node.active; })) - identities.size() },
        { "uncompared_other_edges", unowned_edges },
        { "full_dependency_coverage", false },
    };
}

void append_operators(std::vector<PhaseOperator> & work, const std::vector<size_t> & nodes, int rank, const std::string & request, const std::string & phase,
                      const std::string & operation, const std::map<size_t, uint64_t> * rounds = nullptr) {
    for (const auto node : nodes) work.push_back({ node, rank, request, phase, operation, rounds ? rounds->at(node) : 0 });
}

Json allocator_request_row(const model::HiCacheAllocatorRequest & request, uint64_t page_size) {
    const auto regions = extend_page_regions(request.sequence_tokens, request.prefix_tokens, page_size);
    return {
        {           "request_id",                                                         request.request_id },
        {      "sequence_tokens",                                                    request.sequence_tokens },
        {        "prefix_tokens",                                                      request.prefix_tokens },
        { "existing_tail_tokens",                                               regions.existing_tail_tokens },
        {       "new_full_pages",                                                     regions.new_full_pages },
        {      "new_tail_tokens",                                                    regions.new_tail_tokens },
        {     "decode_iteration", request.decode_iteration ? Json(*request.decode_iteration) : Json(nullptr) },
    };
}

Json submitted_region_row(const core::DagGraph & graph, const source_dag::SubmittedCallRegion & region) {
    const auto & event = graph.runtime_observations().at(region.observation);
    const auto boundary = [&](const std::vector<size_t> & edges) {
        Json rows = Json::array();
        for (const auto id : edges) {
            const auto & edge = graph.edge(id);
            rows.push_back({
                { "source_edge",                          id },
                {        "from",                    edge.src },
                {          "to",                    edge.dst },
                {        "kind", static_cast<int>(edge.kind) }
            });
        }
        return rows;
    };
    return {
        {    "source_observation",                  region.observation },
        {                   "pid",                           event.pid },
        {                   "tid",                           event.tid },
        {       "source_begin_us",                            event.ts },
        {  "observed_interval_us",                           event.dur },
        {      "caller_ownership",                region.caller.status },
        {          "caller_nodes",        region.caller.owned_node_ids },
        {      "ownership_reason",                region.caller.reason },
        {          "uncovered_us", region.caller.uncovered_duration_us },
        {       "submitted_nodes",              region.submitted_nodes },
        { "incoming_dependencies",     boundary(region.incoming_edges) },
        { "outgoing_dependencies",     boundary(region.outgoing_edges) }
    };
}

Json operator_envelope_counts(const core::DagGraph & graph, std::span<const size_t> observations) {
    std::map<std::string, size_t> counts;
    for (const auto observation : observations) ++counts[graph.runtime_observations().at(observation).name];

    return {
        { "status",                                                  counts.empty() ? "unavailable" : "observed" },
        { "counts",                                                                                       counts },
        {  "scope", "original nested CPU operator envelopes; not additive service costs or native kernel counts" },
    };
}

Json observed_allocator_work(const core::DagGraph & graph, const modules::hicache::HiCachePhaseObservationAudit & phases) {
    std::map<std::string, int> ranks;
    for (const auto & phase : phases.observations) ranks.emplace(phase.pid, phase.logical_input);

    // The outer allocation includes eviction. Queue state must come from the
    // inner allocator, after that eviction, on the same thread. A nearby call
    // is not evidence of ownership, nor is the outer entry's free-page count.
    using Lane = std::pair<std::string, std::string>;
    std::map<Lane, std::vector<const core::TraceEvent *>> allocators;
    for (const auto & event : graph.runtime_observations())
        if (event.name == "runtime.hicache.extend_allocator" || event.name == "runtime.hicache.decode_allocator")
            allocators[{ event.pid, event.tid }].push_back(&event);
    for (auto & [lane, events] : allocators) std::ranges::stable_sort(events, {}, [](const auto * event) { return event->ts; });

    Json calls = Json::array();
    size_t unobservable = 0, unobservable_queues = 0;
    for (const auto & event : graph.runtime_observations()) {
        if (event.name != "runtime.hicache.extend_allocation" && event.name != "runtime.hicache.decode_allocation") continue;
        const bool decode = event.name == "runtime.hicache.decode_allocation";
        const bool complete = std::ranges::all_of(std::initializer_list<const char *>{ "request_ids",
                                                                                       "prefix_lens_tokens",
                                                                                       "sequence_lens_tokens",
                                                                                       "page_size_tokens",
                                                                                       "extend_tokens",
                                                                                       "new_pages" },
                                                  [&](const auto * field) { return event.has_arg_key_hint(field); });
        if (event.arg("status") != "returned" || !ranks.contains(event.pid) || !complete
            || (decode && (event.arg("token_per_req") != "1" || !event.has_arg_key_hint("requests")))) {
            ++unobservable;
            continue;
        }

        const auto ids = Json::parse(event.arg("request_ids")).get<std::vector<std::string>>();
        const auto prefixes = Json::parse(event.arg("prefix_lens_tokens")).get<std::vector<uint64_t>>();
        const auto sequences = Json::parse(event.arg("sequence_lens_tokens")).get<std::vector<uint64_t>>();
        const auto page_size = event.arg_u64("page_size_tokens");
        if (!page_size || ids.empty() || prefixes.size() != ids.size() || sequences.size() != ids.size()) {
            ++unobservable;
            continue;
        }

        Json requests = Json::array();
        const auto decode_requests = decode ? Json::parse(event.arg("requests")) : Json::array();
        if (decode && decode_requests.size() != ids.size()) {
            ++unobservable;
            continue;
        }
        bool valid = true;
        for (size_t i = 0; i < ids.size(); ++i) {
            if (prefixes[i] > sequences[i]) {
                valid = false;
                break;
            }
            std::optional<uint64_t> iteration;
            if (decode) {
                const auto & request = decode_requests[i];
                if (request.at("request_id") != ids[i] || request.at("kv_allocated_len") != prefixes[i]) {
                    valid = false;
                    break;
                }
                iteration = request.at("decode_batch_idx").get<uint64_t>();
            }
            requests.push_back(allocator_request_row({ ids[i], sequences[i], prefixes[i], iteration }, page_size));
        }
        if (!valid) {
            ++unobservable;
            continue;
        }

        Json queue = nullptr;
        const auto lane = allocators.find({ event.pid, event.tid });
        std::vector<const core::TraceEvent *> nested;
        const auto end = core::checked_add_u64(event.ts, event.dur, "Allocator observation end overflow");
        if (lane != allocators.end()) {
            auto child = std::ranges::lower_bound(lane->second, event.ts, {}, [](const auto * item) { return item->ts; });
            for (; child != lane->second.end() && (*child)->ts <= end; ++child)
                if ((*child)->name == (decode ? "runtime.hicache.decode_allocator" : "runtime.hicache.extend_allocator")
                    && core::checked_add_u64((*child)->ts, (*child)->dur, "Allocator observation end overflow") <= end)
                    nested.push_back(*child);
        }
        if (nested.size() == 1) {
            const auto & child = *nested.front();
            const bool observed = std::ranges::all_of(
                std::initializer_list<const char *>{ "free_pages_before", "release_pages_before", "free_pages_after", "release_pages_after" },
                [&](const auto * field) { return child.has_arg_key_hint(field); });
            if (observed && child.arg("status") == "returned" && child.arg("allocated") == "true" && child.arg_u64("page_size_tokens") == page_size
                && child.arg_u64("batch_size") == ids.size() && (decode || child.arg_u64("extend_tokens") == event.arg_u64("extend_tokens"))) {
                queue = {
                    {    "free_pages_before",    child.arg_u64("free_pages_before") },
                    { "release_pages_before", child.arg_u64("release_pages_before") },
                    {     "free_pages_after",     child.arg_u64("free_pages_after") },
                    {  "release_pages_after",  child.arg_u64("release_pages_after") },
                };
            }
        }
        if (queue.is_null()) ++unobservable_queues;

        calls.push_back({
            { "source_observation", nested.size() == 1 ? Json(static_cast<size_t>(nested.front() - graph.runtime_observations().data())) : Json(nullptr) },
            {      "logical_input",                                                                                                  ranks.at(event.pid) },
            {   "page_size_tokens",                                                                                                            page_size },
            {         "batch_size",                                                                                                           ids.size() },
            {              "phase",                                                                                        decode ? "decode" : "prefill" },
            {      "extend_tokens",                                                                                       event.arg_u64("extend_tokens") },
            {          "new_pages",                                                                                           event.arg_u64("new_pages") },
            {           "requests",                                                                                                  std::move(requests) },
            {        "queue_state",                                                                                                     std::move(queue) },
        });
    }
    return {
        {                    "calls",                                                                                              std::move(calls) },
        {       "unobservable_calls",                                                                                                  unobservable },
        { "unobservable_queue_calls",                                                                                           unobservable_queues },
        {                    "scope", "observed allocation lengths, index regions and inner queue transitions; not emitted operator correspondence" }
    };
}

Json observed_allocator_operations(const modules::hicache::patch::HiCacheSourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues,
                                   const Json & observed_work) {
    const auto & graph = source.graph();
    std::map<size_t, Json> identities;
    std::set<size_t> ambiguous;
    for (const auto & call : observed_work.at("calls")) {
        if (call.at("source_observation").is_null()) continue;
        const auto index = call.at("source_observation").get<size_t>();
        Json requests = Json::array();
        for (const auto & request : call.at("requests"))
            requests.push_back({
                { "request_id", request.at("request_id") },
                { "iteration", request.at("decode_iteration") }
            });
        const auto identity = Json::array({ call.at("logical_input"), requests, call.at("phase") == "prefill" ? "extend" : "decode" });
        if (!identities.emplace(index, identity).second) ambiguous.insert(index);
    }
    for (const auto index : ambiguous) identities.erase(index);

    ObservedTensorOperations result;
    for (const auto & call : source_dag::observe_allocator_programs(source, queues)) {
        for (const auto node : call.region.work.submitted_nodes)
            if (!graph.node(node).is_cpu) result.call_devices.insert(node);
        const auto identity = identities.find(call.region.work.observation);
        if (!call.program || identity == identities.end()) {
            ++result.unavailable_programs;
            continue;
        }

        result.append(source, queues, *call.program, call.matched_operators, identity->second);
    }

    auto report = std::move(result).report(graph);
    report["source_observation_issues"] = {
        { "unobservable_outer_calls", observed_work.at("unobservable_calls") },
        { "ambiguous_inner_calls", ambiguous.size() }
    };
    report["scope"] =
        "allocator tensor work corroborated by independent call envelopes and device submissions; CPU/control and internal launch dependencies are not fully compared";
    return report;
}

} // namespace

Json observed_phase_structure_report(const core::DagGraph & graph, const modules::hicache::HiCachePhaseObservationAudit & phases) {
    std::vector<PhaseOperator> work;
    size_t unsupported_batches = 0;
    for (const auto & item : phases.observations) {
        if (item.request_ids.size() != 1) {
            ++unsupported_batches;
            continue;
        }
        const auto append = [&](const auto & nodes, const std::string & phase, const std::string & operation) {
            append_operators(work,
                             nodes,
                             item.logical_input,
                             item.request_ids.front(),
                             phase,
                             operation,
                             phase == "decode" ? &item.decode_node_iterations : nullptr);
        };
        append(item.prefill_common_kernel_node_ids, "prefill", "prefill.common");
        append(item.prefill_prefix_attention_node_ids, "prefill", "prefill.attention");
        append(item.prefill.collective_node_ids, "prefill", "prefill.collective");
        append(item.prefill.submit_cpu_node_ids, "prefill", "prefill.submit");
        for (const auto node : item.decode_kernel_node_ids)
            append(std::vector<size_t>{ node },
                   "decode",
                   modules::hicache::is_hicache_paged_attention(graph.event_for_node(node).name) ? "decode.attention" : "decode.common");
        append(item.decode.collective_node_ids, "decode", "decode.collective");
        append(item.decode.submit_cpu_node_ids, "decode", "decode.submit");
    }
    auto report = operator_structure(graph, std::move(work));
    report["unsupported_batches"] = unsupported_batches;
    report["observation_status"] = phases.status;
    const auto collectives = source_dag::observe_device_collectives(graph, phases, &report["device_collective_observations"]);
    report["collective_operations"] = observed_collective_structure(graph, phases, collectives);
    report["allocator_work"] = observed_allocator_work(graph, phases);
    const modules::hicache::patch::HiCacheSourceDagIndex source(graph);
    const auto queues = simulation::detail::discover_cpu_task_queues(graph);
    Json allocator_regions = Json::array();
    for (const auto & region : source_dag::observe_allocator_regions(source, queues)) {
        auto row = submitted_region_row(graph, region.work);
        row["phase"] = region.phase == model::HiCacheAllocatorPhase::Extend ? "prefill" : "decode";
        row["allocated"] = region.allocated ? Json(*region.allocated) : Json(nullptr);
        row["torch_operator_envelopes"] = operator_envelope_counts(graph, region.operator_observations);
        allocator_regions.push_back(std::move(row));
    }
    report["allocator_regions"] = {
        { "status",                                                                             allocator_regions.empty() ? "unavailable" : "observed" },
        {  "calls",                                                                                                       std::move(allocator_regions) },
        {  "scope", "source allocator envelopes, submission/task closure and boundary dependencies; not target generation or replacement safety proof" }
    };
    Json attention = Json::array();
    const auto tensor_operations = [](const std::vector<TensorOperation> & program) {
        Json operations = Json::array();
        for (const auto & operation : program)
            operations.push_back({
                {      "operation",                                                 operation.work.operation },
                { "implementation",                                            operation.work.implementation },
                {      "execution",                               tensor_execution_name(operation.execution) },
                {    "layer_index",     operation.layer_index ? Json(*operation.layer_index) : Json(nullptr) },
                {       "features",                                                  operation.work.features },
                {   "predecessors",                                                   operation.predecessors },
                {  "request_index", operation.request_index ? Json(*operation.request_index) : Json(nullptr) }
            });
        return operations;
    };
    const auto append_attention = [&](const auto & events, const char * collection) {
        for (size_t i = 0; i < events.size(); ++i) {
            const auto & event = events[i];
            if (event.name != "runtime.phase.attention") continue;

            Json dispatch = nullptr;
            Json tensor_program = nullptr;
            Json shape_rule = nullptr;
            if (const auto conditions = source_dag::attention_conditions(event)) {
                if (const auto selected = ascend_attention_dispatch(*conditions))
                    dispatch = {
                        {      "branch",                                                         selected->branch },
                        {      "layout",                                                         selected->layout },
                        { "per_request",                                                    selected->per_request },
                        {       "scope", "framework dispatch only; tensor program and native nodes not generated" }
                    };

                const auto batch = source_dag::attention_batch_shape(event);
                const auto write = source_dag::attention_cache_write(event);
                if (batch && write) {
                    if (const auto padding = source_dag::attention_eager_padding(event))
                        if (const auto reconstructed =
                                target_eager_attention_inputs(*conditions, *batch, *write, *padding, batch->requests, batch->page_tokens))
                            shape_rule = {
                                {      "padded_query_tokens",                                         reconstructed->batch.padded_query_tokens },
                                { "matches_observed_padding",           reconstructed->batch.padded_query_tokens == batch->padded_query_tokens },
                                {                    "scope", "framework eager padding rule on observed inputs; not target execution evidence" }
                            };

                    if (const auto program = ascend_attention_tensor_program(*conditions, *batch, *write)) {
                        tensor_program = {
                            { "operations",tensor_operations(*program)             },
                            {      "scope",
                             "framework cache-write/score/output program derived from observed metadata; excludes caller CPU work, not a native DAG or target "
                             "prediction" }
                        };
                    }
                }
            }

            // Descriptor references are process/phase scoped, not graph node
            // ids. Timing mode retains every formal call; sparse mode does not
            // establish complete region coverage. Neither measures GPU service.
            attention.push_back({
                {        "source_collection",                          collection },
                {       "source_observation",                                   i },
                {                      "pid",                           event.pid },
                {                      "tid",                           event.tid },
                {          "source_begin_us",                            event.ts },
                {     "observed_interval_us",                           event.dur },
                {               "descriptor", Json::parse(event.args_json_view()) },
                {       "framework_dispatch",                 std::move(dispatch) },
                { "framework_tensor_program",           std::move(tensor_program) },
                {         "eager_shape_rule",               std::move(shape_rule) }
            });
        }
    };
    append_attention(graph.prelude_context_events(), "prelude");
    append_attention(graph.runtime_observations(), "formal");
    Json attention_regions = Json::array();
    const auto decode_contexts = source_dag::decode_operator_contexts(phases, source_dag::observe_decode_contexts(graph, phases));
    for (const auto & region : source_dag::observe_attention_regions(source, queues)) {
        auto row = submitted_region_row(graph, region.work);
        row["phase"] = region.phase;
        row["sampling"] = region.sampling;
        row["status"] = region.status;
        row["torch_operator_envelopes"] = operator_envelope_counts(graph, region.operator_observations);
        row["descriptor_position"] = region.descriptor
            ? Json{{"source_collection", region.descriptor->prelude ? "prelude" : "formal"},
                   {"source_observation", region.descriptor->observation}} : Json(nullptr);
        row["decode_context"] = nullptr;
        row["framework_tensor_program"] = nullptr;
        if (const auto context = source_dag::attention_decode_context(region, decode_contexts)) {
            row["decode_context"] = {
                {            "pid",            context->pid },
                {        "request",        context->request },
                {      "iteration",      context->iteration },
                { "context_tokens", context->context_tokens }
            };
            if (region.descriptor) {
                const auto & descriptor = region.descriptor->prelude ? graph.prelude_context_events().at(region.descriptor->observation)
                                                                     : graph.runtime_observations().at(region.descriptor->observation);
                const auto conditions = source_dag::attention_conditions(descriptor);
                const auto batch = source_dag::attention_batch_shape(descriptor, context->context_tokens);
                const auto write = source_dag::attention_cache_write(descriptor);
                if (conditions && batch && write)
                    if (const auto program = ascend_attention_tensor_program(*conditions, *batch, *write))
                        row["framework_tensor_program"] = {
                            { "operations",                                                                                      tensor_operations(*program) },
                            {      "scope", "framework program from the observed call's own scalar-confirmed decode round; not native target DAG generation" }
                        };
            }
        }
        attention_regions.push_back(std::move(row));
    }
    report["attention_backend_observations"] = {
        {  "status",attention.empty() ? "unavailable" : "observed"                    },
        {   "calls",                           std::move(attention) },
        { "regions",                   std::move(attention_regions) },
        {   "scope",
         "source backend descriptors and optional timing-mode call boundaries; references use pid/phase/descriptor_id, not generated target operators or "
         "device service"                                          }
    };
    std::map<size_t, std::string> call_owners;
    report["phase_tensor_operations"] = observed_phase_tensor_operations(source, queues, phases, call_owners);
    for (auto & operation : report["operators"]) {
        const auto owner = call_owners.find(operation.at("graph_node").get<size_t>());
        operation["tensor_call_identity"] = owner == call_owners.end() ? "" : owner->second;
    }
    report["phase_tensor_operations"]["ambiguous_call_ownership_nodes"] =
        std::ranges::count_if(call_owners, [](const auto & row) { return row.second.empty(); });
    report["allocator_tensor_operations"] = observed_allocator_operations(source, queues, report["allocator_work"]);
    return report;
}

Json effect_rows(const model::HiCacheEffectDecisionLedger & ledger) {
    Json rows = Json::array();
    for (const auto & effect : ledger.decisions) {
        Json batches = Json::array();
        for (const auto & batch : effect.storage_service_batches)
            batches.push_back({ batch.operation_index, batch.page_count, batch.existing_page_count, batch.new_page_count });

        using Direction = model::HiCacheTransferDirection;
        size_t operations = 0;
        if (effect.direction == Direction::StorageToHost || effect.direction == Direction::HostToStorage) operations = effect.storage_service_batches.size();
        else if ((effect.direction == Direction::HostToDevice || effect.direction == Direction::DeviceToHost) && effect.effective_page_count > 0)
            operations = effect.operation_ids.size();
        rows.push_back({
            {                   "effect_key",                                                     effect.effect_key },
            {            "effect_family_key",                                              effect.effect_family_key },
            {                  "effect_type",                   model::hicache_effect_type_name(effect.effect_type) },
            {                    "direction",              model::hicache_transfer_direction_name(effect.direction) },
            {                  "cache_scope",                                                    effect.cache_scope },
            {          "target_effect_state",   model::hicache_target_effect_state_name(effect.target_effect_state) },
            {         "schedule_sensitivity", model::hicache_schedule_sensitivity_name(effect.schedule_sensitivity) },
            {                "consumer_role",                             effect.consumer_boundary.source_fact_role },
            {                "resource_lane",                                                  effect.resource_lane },
            { "consumer_dependency_required",                                   effect.consumer_dependency_required },
            {            "eligibility_epoch",                                     effect.eligibility_boundary.epoch },
            {              "operation_count",                                                            operations },
            {         "effective_page_count",                                           effect.effective_page_count },
            {         "completed_page_count",                                           effect.completed_page_count },
            {  "storage_existing_page_count",                                    effect.storage_existing_page_count },
            {       "storage_new_page_count",                                         effect.storage_new_page_count },
            {              "storage_batches",                                                    std::move(batches) },
        });
    }
    return rows;
}

Json execution_structure_report(const core::DagGraph & graph, const model::HiCacheEffectDecisionLedger & effects, const model::HiCachePhaseWorkLedger & phases,
                                std::span<const DeviceCollectivePlan> collectives) {
    Json prefills = Json::array(), decodes = Json::array(), allocations = Json::array();
    std::vector<PhaseOperator> work;
    for (const auto & item : phases.prefills) {
        const auto append = [&](const auto & nodes, const std::string & operation) {
            append_operators(work, nodes, item.logical_input, item.request_id, "prefill", operation);
        };
        append(item.common_kernel_nodes, "prefill.common");
        append(item.prefix_attention_nodes, "prefill.attention");
        append(item.collective_nodes, "prefill.collective");
        append(item.submit_nodes, "prefill.submit");
    }
    for (const auto & item : phases.decodes) {
        const auto append = [&](const auto & nodes, const std::string & operation) {
            append_operators(work, nodes, item.logical_input, item.request_id, "decode", operation, &item.node_iterations);
        };
        for (const auto node : item.kernel_nodes)
            append(std::vector<size_t>{ node },
                   modules::hicache::is_hicache_paged_attention(graph.event_for_node(node).name) ? "decode.attention" : "decode.common");
        append(item.collective_nodes, "decode.collective");
        append(item.submit_nodes, "decode.submit");
    }
    // Retained tensor calls live in copied nodes, not in the disabled original
    // region. Local source references establish this within-run mapping only;
    // cross-trace comparison still uses semantic request/operator identity.
    std::map<size_t, size_t> retained_operators;
    std::map<size_t, size_t> collective_operators;
    for (const auto & node : graph.nodes()) {
        if (!node.active) continue;
        const auto & event = graph.event_for_node(node.id);
        if (event.has_arg_key_hint("collective_source_node")) collective_operators.emplace(event.arg_u64("collective_source_node"), node.id);

        for (const auto * field : { "retained_phase_source_nodes", "retained_allocator_source_nodes" }) {
            if (!event.has_arg_key_hint(field)) continue;
            for (const auto original : Json::parse(event.arg(field)).get<std::vector<size_t>>()) retained_operators.emplace(original, node.id);
        }
    }
    for (auto & item : work) {
        if (const auto collective = collective_operators.find(item.node); collective != collective_operators.end()) {
            // Preserve the phase/request identity, but do not claim that the
            // logical service reproduces the original native HCCL protocol.
            item.node = collective->second;
            item.implementation = "logical_collective_completion";
            continue;
        }

        const auto copied = retained_operators.find(item.node);
        if (copied == retained_operators.end()) continue;
        item.implementation = graph.event_for_node(item.node).name;
        item.source_node = item.node;
        item.node = copied->second;
    }
    for (const auto & item : phases.prefills)
        prefills.push_back({
            {               "logical_input",               item.logical_input },
            {                  "request_id",                  item.request_id },
            {                  "batch_size",                  item.batch_size },
            {            "page_size_tokens",            item.target_page_size },
            {          "prompt_token_count",          item.prompt_token_count },
            { "reusable_prefix_token_count", item.reusable_prefix_token_count },
            {         "prefill_token_count",         item.prefill_token_count },
        });
    for (const auto & item : phases.decodes)
        decodes.push_back({
            {      "logical_input",      item.logical_input },
            {         "request_id",         item.request_id },
            {   "page_size_tokens",   item.target_page_size },
            { "prompt_token_count", item.prompt_token_count },
            {    "iteration_count",    item.iteration_count },
        });

    std::map<std::string, int> allocation_ranks;
    const auto allocator_inputs = source_dag::observe_allocator_inputs(graph);
    for (const auto & prefill : phases.prefills) allocation_ranks.emplace(prefill.pid, prefill.logical_input);
    for (const auto & decode : phases.decodes) allocation_ranks.emplace(decode.pid, decode.logical_input);
    for (const auto & call : phases.allocator_calls) {
        if (!call.formal) continue;

        Json requests = Json::array();
        for (const auto & request : call.requests) requests.push_back(allocator_request_row(request, call.page_size));
        const auto rank = allocation_ranks.find(call.pid);
        Json tensor_program = nullptr;
        const auto inputs = allocator_inputs.find(call.pid);
        if (inputs != allocator_inputs.end()) {
            if (const auto planned = ascend_decode_allocator_program(call, inputs->second)) {
                tensor_program = Json::array();
                for (const auto & operation : *planned)
                    tensor_program.push_back({
                        {      "operation",      operation.work.operation                                           },
                        { "implementation",                                            operation.work.implementation },
                        {       "features",                                                  operation.work.features },
                        {   "predecessors",                                                   operation.predecessors },
                        {      "execution",                                          tensor_execution_name(operation.execution) },
                        {  "request_index", operation.request_index ? Json(*operation.request_index) : Json(nullptr) },
                    });
            }
        }
        Json queue = nullptr;
        if (call.queue_state_known) {
            // State admission already established sufficient capacity. An
            // unbounded/unknown legacy allocator is not a zero-page queue.
            const auto free_after_merge = core::checked_add_u64(call.free_pages_before,
                                                                call.merge_release_queue ? call.release_pages_before : 0,
                                                                "Target allocator queue count overflow");
            queue = {
                {    "free_pages_before",                                   call.free_pages_before },
                { "release_pages_before",                                call.release_pages_before },
                {     "free_pages_after",                  free_after_merge - call.allocated_pages },
                {  "release_pages_after", call.merge_release_queue ? 0 : call.release_pages_before },
            };
        }
        allocations.push_back({
            {          "logical_input",                        rank == allocation_ranks.end() ? -1 : rank->second },
            {                  "phase", call.phase == model::HiCacheAllocatorPhase::Decode ? "decode" : "prefill" },
            {       "page_size_tokens",                                                            call.page_size },
            {             "batch_size",                                                           call.batch_size },
            {          "extend_tokens",                                                        call.extend_tokens },
            {              "new_pages",                                                      call.allocated_pages },
            {      "free_pages_before",                                                    call.free_pages_before },
            {   "release_pages_before",                                                 call.release_pages_before },
            {    "merge_release_queue",                                                  call.merge_release_queue },
            {               "requests",                                                       std::move(requests) },
            {            "queue_state",                                                          std::move(queue) },
            { "planned_tensor_program",                                                 std::move(tensor_program) },
        });
    }
    const auto stats = graph.summary_stats();
    auto collective_report = executed_collective_structure(graph, work, collectives);
    return {
        { "status", "reported" },
        { "scope", "executed HiCache effects and request phase work" },
        { "target_truth_consumed", false },
        { "oracle_cost_consumed", false },
        { "full_graph_correspondence_proven", false },
        { "effect_status", effects.status },
        { "phase_status", phases.status },
        { "effects", effect_rows(effects) },
        { "prefills", prefills },
        { "decodes", decodes },
        { "allocator_work", { { "calls", std::move(allocations) }, { "scope", "target index regions; not proof of emitted allocator operators" } } },
        { "phase_operators", operator_structure(graph, std::move(work)) },
        { "collective_operations", std::move(collective_report) },
        { "executed_phase_tensor_operations", executed_tensor_operations(graph, "phase_tensor") },
        { "executed_allocator_operations", executed_tensor_operations(graph, "allocator", allocation_ranks) },
        { "generated_work_inventory", generated_work_inventory(graph) },
        { "graph",
         { { "active_nodes", stats.active_node_count }, { "active_edges", stats.active_edge_count }, { "edge_kind_counts", stats.edge_counts_by_kind } } },
        { "uncompared",
         { "HiCache CPU/control operator correspondence",
            "identity of intermediate unowned paths",
            "physical resource identity and order involving unowned work" } },
    };
}

} // namespace markov::trace_graph::structure::hicache
