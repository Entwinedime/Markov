#include "markov/trace_graph/modules/hicache/runtime/tensor_program.hpp"
#include "markov/trace_graph/cost/host_operation.hpp"
#include "markov/trace_graph/modules/hicache/runtime/host_program.hpp"
#include "markov/trace_graph/structure/ascend_event.hpp"

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace markov::trace_graph::modules::hicache::runtime {

GeneratedTensorProgram generate_tensor_program(const core::DagGraph & graph, size_t main, size_t worker, size_t compute,
                                               std::span<const structure::TensorOperation> operations, const std::string & identity_prefix,
                                               bool has_layer_load) {
    GeneratedTensorProgram result;
    auto & plan = result.plan;
    std::optional<size_t> previous_host, previous_device, first_device;
    std::vector<size_t> host_returns;
    for (const auto & operation : operations) {
        using Kind = structure::TensorExecutionKind;
        HiCacheHostExpansion segment;
        const auto main_lane = graph.node_lane_key(main);
        std::optional<size_t> previous_main;
        const auto cpu = [&](std::string name) { return segment.append_main(main_lane, std::move(name), previous_main, ""); };
        HostOperationSlots slots{ .main = 0 };
        cost::OperationCostRequest request{ .work = operation.work };
        std::string tail_operation = operation.work.operation + ".return";
        std::string tail_scope = "cpu_service";

        if (operation.execution == Kind::DirectBlockingDevice) tail_scope = "blocking_return_interval";
        if (operation.execution == Kind::BlockingContextReadback) tail_scope = "blocking_readback_interval";
        if (operation.execution == Kind::LayerReady && has_layer_load) {
            auto generated = generate_layer_wait(graph, main, worker, compute);
            slots.main = generated.submit;
            slots.worker = generated.worker;
            slots.device = generated.device;
            slots.tail = generated.tail;
            slots.worker_return = generated.worker_return;
            request.work.operation = "layer_wait.submit";
            request.device_work = structure::ascend_event_work(request.work.component, structure::DeviceEventKind::Wait);
            request.work.implementation = "ascend.stream_wait_event";
            tail_operation = "layer_wait.tail";
            segment = std::move(generated.plan);
            result.event_layers.push_back(operation.layer_index.value());
        }
        else if (operation.execution == Kind::LayerReady || operation.execution == Kind::CpuService) {
            slots.main = cpu("target " + operation.work.operation);
            segment.host_return = cpu("after " + operation.work.operation);
            if (operation.execution == Kind::LayerReady) {
                request.work.operation = "layer_wait.inactive_return";
                request.work.implementation = "inactive";
            }
        }
        else if (operation.execution == Kind::BlockingContextReadback) {
            slots.main = cpu("target " + operation.work.operation + " prepare");
            slots.tail = cpu("target " + operation.work.operation + " synchronous readback");
            segment.host_return = cpu("after " + operation.work.operation);
            // Torch-NPU waits for compute first, then performs aclrtMemcpy
            // on the caller. There is no submitted worker/copy task. Bind the
            // complete post-wait interval, not fictitious device/worker costs.
        }
        else if (operation.execution == Kind::DirectBlockingDevice) {
            slots.main = cpu("target " + operation.work.operation + " submit");
            slots.device = segment.nodes.size();
            segment.nodes.push_back({
                { .name = "target " + operation.work.operation + " device", .is_cpu = false, .lane_key = std::string(graph.node_lane_key(compute)) }
            });
            // The launcher runs on the caller: no queue task or dispatch
            // delay exists. Stream completion releases its synchronous tail.
            // Correlation is reserved for registering worker FIFO tasks in
            // FutureDag. This direct launch has no worker; its causal edge
            // uses the same mutation relation as other generated device work.
            segment.edges.push_back({ slots.main, *slots.device, core::DagEdgeKind::Mutation });
            slots.tail = cpu("target " + tail_operation);
            segment.edges.push_back({ *slots.device, *slots.tail, core::DagEdgeKind::Sync });
            segment.host_return = cpu("after " + operation.work.operation);
        }
        else if (!operation.device_launches.empty()) {
            const auto worker_lane = std::string(graph.node_lane_key(worker));
            const auto device_lane = std::string(graph.node_lane_key(compute));
            slots.main = cpu("target " + operation.work.operation + " submit");
            const auto submitted = cpu("target queued");
            std::optional<size_t> previous_worker, previous_kernel;
            for (const auto & launch : operation.device_launches) {
                const auto launch_worker = segment.nodes.size();
                segment.nodes.push_back({
                    { .name = "target " + launch.operation + " launch", .lane_key = worker_lane },
                    submitted,
                    worker,
                    previous_worker
                });
                const auto kernel = segment.nodes.size();
                segment.nodes.push_back({
                    { .name = "target " + launch.operation, .is_cpu = false, .lane_key = device_lane }
                });
                segment.edges.push_back({ launch_worker, kernel, core::DagEdgeKind::Mutation });
                if (previous_worker) segment.edges.push_back({ *previous_worker, launch_worker, core::DagEdgeKind::Sequential });
                if (previous_kernel) segment.edges.push_back({ *previous_kernel, kernel, core::DagEdgeKind::Stream });
                slots.device_launches.push_back({ launch_worker, kernel });
                request.device_launches.push_back(launch);
                previous_worker = launch_worker;
                previous_kernel = kernel;
            }
            slots.worker = slots.device_launches.front().worker;
            slots.device = previous_kernel;
            slots.worker_return = segment.nodes.size();
            segment.nodes.push_back({
                { .name = "target worker return", .lane_key = worker_lane },
                submitted,
                worker,
                previous_worker
            });
            segment.edges.push_back({ *previous_worker, *slots.worker_return, core::DagEdgeKind::Sequential });
            slots.tail = cpu("target " + tail_operation);
            segment.host_return = cpu("after " + operation.work.operation);
        }
        else {
            auto generated = generate_async_device_operation(graph, main, worker, compute, operation.work.operation);
            slots.main = generated.submit;
            slots.worker = generated.worker;
            slots.device = generated.device;
            segment = std::move(generated.plan);
            slots.worker_return = generated.worker_return;
            // The queued boundary releases the worker before CPU-side return
            // work. Keep that work on its own slot so it can overlap compute.
            slots.tail = segment.host_return;
            segment.nodes[*slots.tail].work.name = "target " + tail_operation;
            previous_main = slots.tail;
            segment.host_return = cpu("after " + operation.work.operation);
        }

        const auto offset = plan.nodes.size();
        slots.main += offset;
        if (slots.worker) *slots.worker += offset;
        if (slots.worker_return) *slots.worker_return += offset;
        if (slots.device) *slots.device += offset;
        if (slots.tail) *slots.tail += offset;
        const auto host_return = offset + segment.host_return;
        for (auto & launch : slots.device_launches) {
            launch.worker += offset;
            launch.device += offset;
        }

        for (size_t i = 0; i < segment.nodes.size(); ++i) {
            auto & node = segment.nodes[i];
            if (node.submission) *node.submission += offset;
            if (node.task_predecessor) *node.task_predecessor += offset;
            node.work.category = "phase_patch";
            node.work.attrs["component"] = operation.work.component;
            node.work.attrs["operation"] = operation.work.operation;
            node.work.attrs["implementation"] = operation.work.implementation;
            node.work.attrs[identity_prefix + "_operation_index"] = std::to_string(result.operations.size());
            node.work.attrs[identity_prefix + "_execution"] = structure::tensor_execution_name(operation.execution);
            if (operation.request_index) node.work.attrs[identity_prefix + "_request_index"] = std::to_string(*operation.request_index);
            const auto local = offset + i;
            std::string scope;
            if (local == slots.main || (slots.tail && local == *slots.tail)) scope = "cpu_service";
            else if (slots.worker && local == *slots.worker) scope = "worker_launch_service";
            else if (slots.worker_return && local == *slots.worker_return) scope = "worker_return_service";
            else if (slots.device && local == *slots.device) scope = "device_service";
            if (!scope.empty()) {
                if (slots.tail && local == *slots.tail) scope = tail_scope;
                node.work.attrs["cost_operation"] = slots.tail && local == *slots.tail ? tail_operation : request.work.operation;
                node.work.attrs["cost_implementation"] = request.work.implementation;
                node.work.attrs["cost_timing_scope"] = scope;
            }
            for (size_t launch_index = 0; launch_index < slots.device_launches.size(); ++launch_index) {
                const auto & launch = slots.device_launches[launch_index];
                if (local != launch.worker && local != launch.device) continue;

                const auto & launch_work = request.device_launches[launch_index];
                node.work.attrs["cost_operation"] = launch_work.operation;
                node.work.attrs["cost_implementation"] = launch_work.implementation;
                node.work.attrs["cost_timing_scope"] = local == launch.worker ? "worker_launch_service" : "device_service";
                if (local == launch.device && local != *slots.device) node.work.attrs["tensor_intermediate_device"] = "true";
            }
            // Store geometry once per operation, not on every worker, device
            // and boundary node. Call identity is attached by the coordinator.
            if (local == slots.main) node.work.attrs["work_features"] = nlohmann::json(operation.work.features).dump();
            if (operation.execution == Kind::BlockingContextReadback && local == host_return) node.work.attrs["tensor_completion"] = "true";
            plan.nodes.push_back(std::move(node));
        }
        for (const auto & edge : segment.edges) plan.edges.push_back({ offset + edge.from, offset + edge.to, edge.kind });
        for (const auto & wait : segment.event_waits) plan.event_waits.push_back({ wait.source_node, offset + wait.consumer });

        if (previous_host) plan.edges.push_back({ *previous_host, slots.main, core::DagEdgeKind::Sequential });
        if (operation.execution == Kind::BlockingContextReadback) {
            if (previous_device) plan.edges.push_back({ *previous_device, *slots.tail, core::DagEdgeKind::Sync });
            else plan.waits.push_back({ compute, *slots.tail });
        }
        if (slots.device) {
            const auto entry_device = slots.device_launches.empty() ? *slots.device : slots.device_launches.front().device;
            if (previous_device) plan.edges.push_back({ *previous_device, entry_device, core::DagEdgeKind::Stream });
            for (const auto predecessor : operation.predecessors) {
                const auto & source = result.operations.at(predecessor).slots.front();
                const auto producer = source.device.value_or(host_returns.at(predecessor));
                if (previous_device && producer == *previous_device) continue;
                plan.edges.push_back({ producer, entry_device, core::DagEdgeKind::Mutation });
            }
            if (!first_device) first_device = entry_device;
            previous_device = slots.device;
        }
        else {
            // Explicit host dependencies are part of the structure contract
            // too. A readback can prepare its arguments before data is ready,
            // but its synchronous copy cannot start before its producers.
            const auto consumer = operation.execution == Kind::BlockingContextReadback ? *slots.tail : slots.main;
            for (const auto predecessor : operation.predecessors) {
                const auto & source = result.operations.at(predecessor).slots.front();
                const auto producer = source.device.value_or(host_returns.at(predecessor));
                if (operation.execution == Kind::BlockingContextReadback && previous_device && producer == *previous_device) continue;
                const auto kind = source.device ? core::DagEdgeKind::Sync : core::DagEdgeKind::Mutation;
                plan.edges.push_back({ producer, consumer, kind });
            }
        }

        // Host-only work has no implicit device barrier. Its CPU return still
        // precedes the next submission, while existing compute stays in flight.
        plan.host_return = host_return;
        previous_host = plan.host_return;
        request.worker = slots.worker.has_value();
        request.device = slots.device.has_value();
        if (slots.tail) {
            auto tail = request.work;
            tail.operation = tail_operation;
            tail.timing_scope = tail_scope;
            request.return_work = std::move(tail);
        }

        result.operations.push_back({ std::move(request), { std::move(slots) } });
        host_returns.push_back(host_return);
    }

    if (first_device) plan.streams.push_back({ compute, *first_device, *previous_device });
    return result;
}

} // namespace markov::trace_graph::modules::hicache::runtime
