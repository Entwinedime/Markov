#include "markov/trace_graph/modules/hicache/runtime/tensor_program.hpp"
#include "markov/trace_graph/cost/host_operation.hpp"
#include "markov/trace_graph/modules/hicache/runtime/load_execution.hpp"
#include "markov/trace_graph/structure/ascend_event.hpp"

#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace markov::trace_graph::modules::hicache::runtime {

GeneratedTensorProgram generate_tensor_program(const core::DagGraph & graph, size_t main, size_t worker, size_t compute,
                                               std::span<const structure::TensorOperation> operations, const std::string & identity_prefix, bool has_layer_load) {
    GeneratedTensorProgram result;
    auto & plan = result.plan;
    std::optional<size_t> previous_host, previous_device, first_device;
    for (const auto & operation : operations) {
        using Kind = structure::TensorExecutionKind;
        HiCacheHostExpansion segment;
        TensorOperationSlots slots{ .submit = 0, .host_return = 0, .work = operation.work };
        if (operation.execution == Kind::DirectBlockingDevice) slots.tail_scope = "blocking_return_interval";
        if (operation.execution == Kind::BlockingContextReadback) slots.tail_scope = "blocking_readback_interval";
        if (operation.execution == Kind::LayerReady && has_layer_load) {
            auto generated = generate_layer_wait(graph, main, worker, compute);
            slots.submit = generated.submit;
            slots.worker = generated.worker;
            slots.device = generated.device;
            slots.tail = generated.tail;
            slots.worker_return = generated.worker_return;
            slots.work.operation = "layer_wait.submit";
            slots.device_work = structure::ascend_event_work(slots.work.component, structure::DeviceEventKind::Wait);
            slots.work.implementation = "ascend.stream_wait_event";
            slots.tail_operation = "layer_wait.tail";
            segment = std::move(generated.plan);
            result.event_layers.push_back(operation.layer_index.value());
        }
        else if (operation.execution == Kind::LayerReady || operation.execution == Kind::CpuService) {
            const auto lane = std::string(graph.node_lane_key(main));
            segment.nodes.push_back({
                { .name = "target " + operation.work.operation, .lane_key = lane }
            });
            segment.nodes.push_back({
                { .name = "after " + operation.work.operation, .lane_key = lane }
            });
            segment.edges.push_back({ 0, 1, core::DagEdgeKind::Sequential });
            segment.host_return = 1;
            if (operation.execution == Kind::LayerReady) {
                slots.work.operation = "layer_wait.inactive_return";
                slots.work.implementation = "inactive";
            }
        }
        else if (operation.execution == Kind::BlockingContextReadback) {
            const auto lane = std::string(graph.node_lane_key(main));
            slots.tail = 1;
            slots.tail_operation = operation.work.operation + ".return";
            segment.nodes.push_back({
                { .name = "target " + operation.work.operation + " prepare", .lane_key = lane }
            });
            segment.nodes.push_back({
                { .name = "target " + operation.work.operation + " synchronous readback", .lane_key = lane }
            });
            segment.nodes.push_back({
                { .name = "after " + operation.work.operation, .lane_key = lane }
            });
            segment.edges = {
                { 0, 1, core::DagEdgeKind::Sequential },
                { 1, 2, core::DagEdgeKind::Sequential }
            };
            segment.host_return = 2;
            // Torch-NPU waits for compute first, then performs aclrtMemcpy
            // on the caller. There is no submitted worker/copy task. Bind the
            // complete post-wait interval, not fictitious device/worker costs.
        }
        else if (operation.execution == Kind::DirectBlockingDevice) {
            const auto lane = std::string(graph.node_lane_key(main));
            slots.device = 1;
            slots.tail = 2;
            slots.tail_operation = operation.work.operation + ".return";
            segment.nodes.push_back({
                { .name = "target " + operation.work.operation + " submit", .lane_key = lane }
            });
            segment.nodes.push_back({
                { .name = "target " + operation.work.operation + " device", .is_cpu = false, .lane_key = std::string(graph.node_lane_key(compute)) }
            });
            segment.nodes.push_back({
                { .name = "target " + slots.tail_operation, .lane_key = lane }
            });
            segment.nodes.push_back({
                { .name = "after " + operation.work.operation, .lane_key = lane }
            });
            // The launcher runs on the caller: no queue task or dispatch
            // delay exists. Stream completion releases its synchronous tail.
            // Correlation is reserved for registering worker FIFO tasks in
            // FutureDag. This direct launch has no worker; its causal edge
            // uses the same mutation relation as other generated device work.
            segment.edges = {
                { 0, 1,   core::DagEdgeKind::Mutation },
                { 0, 2, core::DagEdgeKind::Sequential },
                { 1, 2,       core::DagEdgeKind::Sync },
                { 2, 3, core::DagEdgeKind::Sequential }
            };
            segment.host_return = 3;
        }
        else if (!operation.device_launches.empty()) {
            const auto main_lane = std::string(graph.node_lane_key(main));
            const auto worker_lane = std::string(graph.node_lane_key(worker));
            const auto device_lane = std::string(graph.node_lane_key(compute));
            segment.nodes.push_back({
                { .name = "target " + operation.work.operation + " submit", .lane_key = main_lane }
            });
            segment.nodes.push_back({
                { .name = "target queued", .lane_key = main_lane }
            });
            segment.edges.push_back({ 0, 1, core::DagEdgeKind::Sequential });
            std::optional<size_t> previous_worker, previous_kernel;
            for (const auto & launch : operation.device_launches) {
                const auto launch_worker = segment.nodes.size();
                segment.nodes.push_back({
                    { .name = "target " + launch.operation + " launch", .lane_key = worker_lane },
                    1,
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
                slots.device_launches.push_back({ launch_worker, kernel, launch });
                previous_worker = launch_worker;
                previous_kernel = kernel;
            }
            slots.worker = slots.device_launches.front().worker;
            slots.device = previous_kernel;
            slots.worker_return = segment.nodes.size();
            segment.nodes.push_back({
                { .name = "target worker return", .lane_key = worker_lane },
                1,
                worker,
                previous_worker
            });
            segment.edges.push_back({ *previous_worker, *slots.worker_return, core::DagEdgeKind::Sequential });
            slots.tail = segment.nodes.size();
            slots.tail_operation = operation.work.operation + ".return";
            segment.nodes.push_back({
                { .name = "target " + slots.tail_operation, .lane_key = main_lane }
            });
            segment.edges.push_back({ 1, *slots.tail, core::DagEdgeKind::Sequential });
            segment.host_return = segment.nodes.size();
            segment.nodes.push_back({
                { .name = "after " + operation.work.operation, .lane_key = main_lane }
            });
            segment.edges.push_back({ *slots.tail, segment.host_return, core::DagEdgeKind::Sequential });
        }
        else {
            auto generated = generate_async_device_operation(graph, main, worker, compute, operation.work.operation);
            slots.submit = generated.submit;
            slots.worker = generated.worker;
            slots.device = generated.device;
            segment = std::move(generated.plan);
            slots.worker_return = generated.worker_return;
            // The queued boundary releases the worker before CPU-side return
            // work. Keep that work on its own slot so it can overlap compute.
            slots.tail = segment.host_return;
            slots.tail_operation = operation.work.operation + ".return";
            segment.nodes[*slots.tail].work.name = "target " + slots.tail_operation;
            const auto returned = segment.nodes.size();
            segment.nodes.push_back({
                { .name = "after " + operation.work.operation, .lane_key = std::string(graph.node_lane_key(main)) }
            });
            segment.edges.push_back({ *slots.tail, returned, core::DagEdgeKind::Sequential });
            segment.host_return = returned;
        }
        const auto offset = plan.nodes.size();
        slots.submit += offset;
        if (slots.worker) *slots.worker += offset;
        if (slots.worker_return) *slots.worker_return += offset;
        if (slots.device) *slots.device += offset;
        if (slots.tail) *slots.tail += offset;
        slots.host_return = offset + segment.host_return;
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
            if (local == slots.submit || (slots.tail && local == *slots.tail)) scope = "cpu_service";
            else if (slots.worker && local == *slots.worker) scope = "worker_launch_service";
            else if (slots.worker_return && local == *slots.worker_return) scope = "worker_return_service";
            else if (slots.device && local == *slots.device) scope = "device_service";
            if (!scope.empty()) {
                if (slots.tail && local == *slots.tail) scope = slots.tail_scope;
                node.work.attrs["cost_operation"] = slots.tail && local == *slots.tail ? slots.tail_operation : slots.work.operation;
                node.work.attrs["cost_implementation"] = slots.work.implementation;
                node.work.attrs["cost_timing_scope"] = scope;
            }
            for (const auto & launch : slots.device_launches) {
                if (local != launch.worker && local != launch.device) continue;
                node.work.attrs["cost_operation"] = launch.work.operation;
                node.work.attrs["cost_implementation"] = launch.work.implementation;
                node.work.attrs["cost_timing_scope"] = local == launch.worker ? "worker_launch_service" : "device_service";
                if (local == launch.device && local != *slots.device) node.work.attrs["tensor_intermediate_device"] = "true";
            }
            // Store geometry once per operation, not on every worker, device
            // and boundary node. Call identity is attached by the coordinator.
            if (local == slots.submit) node.work.attrs["work_features"] = nlohmann::json(operation.work.features).dump();
            if (operation.execution == Kind::BlockingContextReadback && local == slots.host_return) node.work.attrs["tensor_completion"] = "true";
            plan.nodes.push_back(std::move(node));
        }
        for (const auto & edge : segment.edges) plan.edges.push_back({ offset + edge.from, offset + edge.to, edge.kind });
        for (const auto & wait : segment.event_waits) plan.event_waits.push_back({ wait.source_node, offset + wait.consumer });

        if (previous_host) plan.edges.push_back({ *previous_host, slots.submit, core::DagEdgeKind::Sequential });
        if (operation.execution == Kind::BlockingContextReadback) {
            if (previous_device) plan.edges.push_back({ *previous_device, *slots.tail, core::DagEdgeKind::Sync });
            else plan.waits.push_back({ compute, *slots.tail });
        }
        if (slots.device) {
            const auto entry_device = slots.device_launches.empty() ? *slots.device : slots.device_launches.front().device;
            if (previous_device) plan.edges.push_back({ *previous_device, entry_device, core::DagEdgeKind::Stream });
            for (const auto predecessor : operation.predecessors) {
                const auto & source = result.operations.at(predecessor);
                const auto producer = source.device.value_or(source.host_return);
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
            const auto consumer = operation.execution == Kind::BlockingContextReadback ? *slots.tail : slots.submit;
            for (const auto predecessor : operation.predecessors) {
                const auto & source = result.operations.at(predecessor);
                const auto producer = source.device.value_or(source.host_return);
                if (operation.execution == Kind::BlockingContextReadback && previous_device && producer == *previous_device) continue;
                const auto kind = source.device ? core::DagEdgeKind::Sync : core::DagEdgeKind::Mutation;
                plan.edges.push_back({ producer, consumer, kind });
            }
        }

        // Host-only work has no implicit device barrier. Its CPU return still
        // precedes the next submission, while existing compute stays in flight.
        plan.host_return = slots.host_return;
        previous_host = plan.host_return;
        result.operations.push_back(std::move(slots));
    }

    if (first_device) plan.streams.push_back({ compute, *first_device, *previous_device });
    return result;
}

std::vector<cost::TensorCostRequest> tensor_cost_requests(std::span<const TensorOperationSlots> operations) {
    std::vector<cost::TensorCostRequest> result;
    result.reserve(operations.size());
    for (const auto & operation : operations) {
        cost::TensorCostRequest request{
            .work = operation.work,
            .worker = operation.worker.has_value(),
            .device = operation.device.has_value(),
        };
        if (operation.tail) {
            auto tail = operation.work;
            tail.operation = operation.tail_operation;
            tail.timing_scope = operation.tail_scope;
            request.return_work = std::move(tail);
        }
        for (const auto & launch : operation.device_launches) request.device_launches.push_back(launch.work);
        request.device_work = operation.device_work;
        result.push_back(std::move(request));
    }
    return result;
}

void bind_tensor_program(GeneratedTensorProgram & generated, std::span<const cost::TensorOperationCost> costs) {
    if (costs.size() != generated.operations.size()) throw std::invalid_argument("Tensor program requires one bound cost record per operation");

    for (size_t i = 0; i < costs.size(); ++i) {
        const auto & slots = generated.operations[i];
        if (slots.device_launches.empty()) {
            bind_host_operation(generated.plan, { slots.submit, slots.worker, slots.device, slots.worker_return }, costs[i].call, "tensor_independent_model");
        }
        else {
            bind_host_operation(generated.plan, { .main = slots.submit, .worker_return = slots.worker_return }, costs[i].call, "tensor_independent_model");
            for (size_t launch = 0; launch < slots.device_launches.size(); ++launch) {
                const auto & target = slots.device_launches[launch];
                const auto & estimate = costs[i].device_launches.at(launch);
                auto & worker = generated.plan.nodes[target.worker].work;
                bind_worker_segment(worker, estimate.worker_us, estimate.residual_us, "tensor_independent_model");
                if (launch == 0) worker.cpu_task_ready_delay_us = costs[i].call.dispatch_us;
                auto & device = generated.plan.nodes[target.device].work;
                device.duration = estimate.device_us;
                device.attrs["cost_binding_source"] = "tensor_independent_model";
            }
        }
        if (slots.tail) { bind_host_operation(generated.plan, { .main = *slots.tail }, costs[i].tail.value(), "tensor_independent_model"); }
    }
}

} // namespace markov::trace_graph::modules::hicache::runtime
