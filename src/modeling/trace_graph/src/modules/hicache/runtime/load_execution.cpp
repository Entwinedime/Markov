#include "markov/trace_graph/modules/hicache/runtime/load_execution.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/structure/ascend_event.hpp"
#include <stdexcept>
#include <string>
#include <utility>

namespace markov::trace_graph::modules::hicache::runtime {
namespace {

struct SubmittedDevice {
    size_t worker, device, returned;
};

structure::OperationWork idle_stream_sync_work(const std::string & component) {
    return { component, "runtime.stream_sync", "ascend.runtime", "idle_stream_sync_interval", { { "previous_work_completed", 1.0 } } };
}

// The queued main-thread boundary releases a worker task; it does not wait
// for the device. The caller connects the returned nodes and stream/event dependencies.
SubmittedDevice append_submitted_device(HiCacheHostExpansion & plan, const core::DagGraph & graph, size_t worker, size_t submitted, std::string_view lane,
                                        std::string name, std::string device_suffix = "") {
    const auto job = plan.nodes.size();
    plan.nodes.push_back({
        { .name = name + " worker", .category = "hicache_patch", .lane_key = std::string(graph.node_lane_key(worker)) },
        submitted,
        worker
    });
    const auto device = plan.nodes.size();
    plan.nodes.push_back({
        { .name = name + device_suffix, .category = "hicache_patch", .is_cpu = false, .lane_key = std::string(lane) }
    });
    const auto returned = plan.nodes.size();
    plan.nodes.push_back({
        { .name = std::move(name) + " worker return", .category = "hicache_patch", .lane_key = std::string(graph.node_lane_key(worker)) },
        submitted,
        worker,
        job
    });
    plan.edges.push_back({ job, returned, core::DagEdgeKind::Sequential });
    return { job, device, returned };
}

} // namespace

void bind_worker_segment(core::DagSyntheticNodeSpec & worker, uint64_t service_us, uint64_t residual_us, std::string_view source) {
    worker.duration = core::checked_add_u64(service_us, residual_us, "Worker segment interval overflow");
    worker.attrs["worker_service_us"] = std::to_string(service_us);
    worker.attrs["worker_residual_us"] = std::to_string(residual_us);
    worker.attrs["cost_binding_source"] = source;
}

void bind_host_operation(HiCacheHostExpansion & plan, const HostOperationSlots & slots, const cost::HostOperationCost & cost, std::string_view source) {
    auto & main = plan.nodes[slots.main].work;
    main.duration = cost.main_us;
    main.cpu_gap_after = cost.main_residual_us;
    main.attrs["cost_binding_source"] = source;

    if (slots.worker) {
        auto & worker = plan.nodes[*slots.worker].work;
        bind_worker_segment(worker, cost.worker_us, cost.worker_residual_us, source);
        worker.cpu_task_ready_delay_us = cost.dispatch_us;
    }
    if (slots.worker_return) {
        auto & returned = plan.nodes[*slots.worker_return].work;
        bind_worker_segment(returned, cost.worker_return_us, cost.worker_return_residual_us, source);
    }
    if (slots.device) {
        auto & device = plan.nodes[*slots.device].work;
        device.duration = cost.device_us;
        device.attrs["cost_binding_source"] = source;
    }
}

void bind_host_operations(HiCacheHostExpansion & plan, std::span<const HostOperationBinding> operations, cost::OperatorCostProvider & provider,
                          std::string_view source) {
    std::vector<cost::OperationCostRequest> requests;
    requests.reserve(operations.size());
    for (const auto & operation : operations) requests.push_back(operation.request);

    const auto costs = cost::estimate_operation_costs(requests, provider);
    for (size_t i = 0; i < operations.size(); ++i) {
        for (const auto & slots : operations[i].slots) {
            if (slots.device_launches.empty()) { bind_host_operation(plan, slots, costs[i].call, source); }
            else {
                bind_host_operation(plan, { .main = slots.main, .worker_return = slots.worker_return }, costs[i].call, source);
                for (size_t launch = 0; launch < slots.device_launches.size(); ++launch) {
                    const auto & target = slots.device_launches[launch];
                    const auto & estimate = costs[i].device_launches[launch];
                    auto & worker = plan.nodes[target.worker].work;
                    bind_worker_segment(worker, estimate.worker_us, estimate.residual_us, source);
                    if (launch == 0) worker.cpu_task_ready_delay_us = costs[i].call.dispatch_us;

                    auto & device = plan.nodes[target.device].work;
                    device.duration = estimate.device_us;
                    device.attrs["cost_binding_source"] = source;
                }
            }
            if (slots.tail) bind_host_operation(plan, { .main = *slots.tail }, costs[i].tail.value(), source);
        }
    }
}

GeneratedPagedRelease generate_paged_release(const core::DagGraph & graph, size_t main, size_t worker, size_t compute, uint64_t released_pages,
                                             uint64_t page_size, uint64_t buffer_pages, bool backed_up, bool need_sort) {
    GeneratedPagedRelease result;
    auto & plan = result.plan;
    const structure::OperationWork work{
        "hicache_direct",
        "",
        std::string(page_size > 1 ? "ascend.paged_immediate." : "ascend.token_immediate.") + (backed_up ? "backup" : "regular"),
        "",
        { { "released_pages", static_cast<double>(released_pages) },
          { "page_size", static_cast<double>(page_size) },
          { "release_buffer_pages", static_cast<double>(buffer_pages) },
          { "need_sort", need_sort ? 1.0 : 0.0 } }
    };

    std::optional<size_t> previous;
    const auto lane = graph.node_lane_key(main);
    const auto cpu = [&](std::string_view operation, const char * name) {
        const auto node = plan.append_main(lane, name, previous);
        auto primitive = work;
        primitive.operation = "release." + std::string(operation);
        result.operations.push_back({ { .work = std::move(primitive) }, { { .main = node } } });
        return node;
    };

    if (page_size > 1) {
        (void)cpu("before_first_sync", "target release index preparation");
        const auto first_sync = cpu("first_sync", "target release index readback");
        result.operations.back().request.main_service_work = idle_stream_sync_work(work.component);
        plan.waits.push_back({ compute, first_sync });

        (void)cpu("before_second_sync", "target release unique pages");
        const auto second_sync = cpu("second_sync", "target release unique-page readback");
        result.operations.back().request.main_service_work = idle_stream_sync_work(work.component);
        plan.waits.push_back({ compute, second_sync });
    }

    (void)cpu("concatenate", "target release concatenate submit");
    const auto queued = plan.append_main(lane, "target release concatenate queued", previous);
    const auto task = append_submitted_device(plan, graph, worker, queued, graph.node_lane_key(compute), "target release concatenate");
    auto & concatenate = result.operations.back();
    concatenate.request.worker = true;
    concatenate.request.device = true;
    concatenate.slots.back().worker = task.worker;
    concatenate.slots.back().device = task.device;
    concatenate.slots.back().worker_return = task.returned;
    plan.edges.push_back({ task.worker, task.device, core::DagEdgeKind::Mutation });
    plan.streams.push_back({ compute, task.device, task.device });

    (void)cpu("tail", "target release bookkeeping");
    plan.host_return = plan.append_main(lane, "target release host return", previous);
    return result;
}

GeneratedLayerWait generate_layer_wait(const core::DagGraph & graph, size_t main, size_t worker, size_t compute) {
    GeneratedLayerWait result;
    auto & plan = result.plan;
    const auto lane = graph.node_lane_key(main);
    std::optional<size_t> previous;
    result.submit = plan.append_main(lane, "target layer event wait submit", previous);
    const auto queued = plan.append_main(lane, "target layer event wait queued", previous);
    const auto task = append_submitted_device(plan, graph, worker, queued, graph.node_lane_key(compute), "target layer event wait");
    result.worker = task.worker;
    result.device = task.device;
    result.worker_return = task.returned;
    plan.edges.push_back({ task.worker, task.device, core::DagEdgeKind::Mutation });
    plan.streams.push_back({ compute, task.device, task.device });

    // Only the device stream waits. Its event is the target layer completion,
    // supplied at execution; the CPU returns after submitting the wait.
    plan.event_waits.push_back({ core::DagNode::kNoNode, plan.streams.front().last });
    result.tail = plan.append_main(lane, "target layer wait bookkeeping", previous);
    plan.host_return = plan.append_main(lane, "target layer wait host return", previous);
    return result;
}

GeneratedTransferSubmission generate_page_first_submission(const core::DagGraph & graph, size_t main, size_t worker, size_t compute, size_t transfer_stream,
                                                           size_t pages, uint64_t page_bytes, size_t completion_count, std::string_view operation,
                                                           bool write_back) {
    if (!pages || !completion_count || !page_bytes || page_bytes % 2 || graph.node(compute).lane_id == graph.node(transfer_stream).lane_id
        || graph.node(transfer_stream).is_cpu || graph.node(main).gpu_id != graph.node(transfer_stream).gpu_id || !graph.node(main).is_cpu
        || !graph.node(worker).is_cpu || graph.node(compute).is_cpu || graph.node(main).lane_id == graph.node(worker).lane_id
        || graph.node(main).gpu_id != graph.node(worker).gpu_id || graph.node(main).gpu_id != graph.node(compute).gpu_id)
        throw std::invalid_argument("Generated Ascend transfer needs whole K/V pages, completions and a separate transfer stream");

    GeneratedTransferSubmission result;
    auto & plan = result.plan;
    const bool load = operation == "load";
    structure::OperationWork work{
        "hicache_io_control",
        "",
        load ? "kernel_ascend.page_first_direct.load"
             : (write_back ? "kernel_ascend.page_first_direct.write_back" : "kernel_ascend.page_first_direct.write_through"),
        "",
        { { "payload_bytes", static_cast<double>(core::checked_multiply_u64(pages, page_bytes, "Transfer payload overflow")) },
          { "page_bytes", static_cast<double>(page_bytes) },
          { "pages", static_cast<double>(pages) } }
    };
    if (load) work.features.emplace("layers", static_cast<double>(completion_count));

    const auto main_lane = std::string(graph.node_lane_key(main));
    const auto compute_lane = std::string(graph.node_lane_key(compute));
    const auto transfer_lane = std::string(graph.node_lane_key(transfer_stream));
    const auto prefix = "target " + std::string(operation);
    std::optional<size_t> previous_main, previous_device;
    const auto cpu = [&](const std::string & name) { return plan.append_main(main_lane, name, previous_main); };
    const auto declare_work = [&](std::string_view name) {
        auto primitive = work;
        primitive.operation = std::string(operation) + ".submission." + std::string(name);
        result.operations.push_back({ { .work = std::move(primitive) }, {} });
        return result.operations.size() - 1;
    };
    const auto operation_cpu = [&](size_t operation_index, const std::string & name) {
        const auto node = cpu(name);
        result.operations[operation_index].slots.push_back({ .main = node });
        return node;
    };
    const auto event = [&](size_t operation_index, const std::string & name, const std::string & lane, structure::DeviceEventKind kind) {
        (void)operation_cpu(operation_index, name + " submit");
        const auto submitted = cpu(name + " queued");
        const auto task = append_submitted_device(plan, graph, worker, submitted, lane, name);
        auto & description = result.operations[operation_index];
        description.request.worker = true;
        description.request.device = true;
        description.request.device_work = structure::ascend_event_work(work.component, kind);
        description.slots.back().worker = task.worker;
        description.slots.back().device = task.device;
        description.slots.back().worker_return = task.returned;
        plan.edges.push_back({ task.worker, task.device, core::DagEdgeKind::Mutation });
        return task.device;
    };

    (void)operation_cpu(declare_work("prepare_sync"), prefix + " before index synchronization");
    const auto index_sync = declare_work("index_sync");
    result.operations[index_sync].request.main_service_work = idle_stream_sync_work(work.component);
    const auto sync = operation_cpu(index_sync, prefix + " index synchronization");
    plan.waits.push_back({ compute, sync });
    const auto ready = event(declare_work("start_record"), prefix + " compute ready", compute_lane, structure::DeviceEventKind::Record);
    plan.streams.push_back({ compute, ready, ready });
    const auto wait = event(declare_work("wait_event"), prefix + " wait for compute", transfer_lane, structure::DeviceEventKind::Wait);
    plan.edges.push_back({ ready, wait, core::DagEdgeKind::Sync });
    previous_device = wait;
    plan.write_start = wait;

    const auto first_copy = declare_work("first_copy");
    const auto copy = declare_work("copy");
    for (size_t page = 0; page < pages; ++page) {
        for (size_t kv = 0; kv < 2; ++kv) {
            (void)operation_cpu(page == 0 && kv == 0 ? first_copy : copy, prefix + " K/V page submission");
            const auto submit = cpu(prefix + " K/V page submitted");
            const auto transfer = plan.nodes.size();
            plan.nodes.push_back({
                { .name = prefix + " K/V page transfer", .category = "hicache_patch", .is_cpu = false, .lane_key = transfer_lane }
            });
            plan.edges.push_back({ submit, transfer, core::DagEdgeKind::Mutation });
            plan.edges.push_back({ *previous_device, transfer, core::DagEdgeKind::Stream });
            plan.payload.emplace(transfer, page_bytes / 2);
            plan.payload_bytes = core::checked_add_u64(plan.payload_bytes, page_bytes / 2, "Transfer payload overflow");
            previous_device = transfer;
        }
    }

    const auto first_record = declare_work(load ? "first_layer_record" : "completion_record");
    std::optional<size_t> later_record;
    for (size_t record_index = 0; record_index < completion_count; ++record_index) {
        if (record_index == 1) later_record = declare_work("layer_record");

        const auto record =
            event(record_index == 0 ? first_record : *later_record, prefix + " transfer complete", transfer_lane, structure::DeviceEventKind::Record);
        plan.edges.push_back({ *previous_device, record, core::DagEdgeKind::Stream });
        previous_device = record;
        result.completion_records.push_back(record);
    }
    plan.completion = *previous_device;
    plan.streams.push_back({ transfer_stream, wait, plan.completion });
    (void)operation_cpu(declare_work("tail"), prefix + " bookkeeping");
    plan.host_return = cpu(prefix + " host return");
    return result;
}

GeneratedDeviceOperation generate_async_device_operation(const core::DagGraph & graph, size_t main_node, size_t worker_node, size_t compute_node,
                                                         std::string_view operation) {
    const auto & main = graph.node(main_node);
    const auto & worker = graph.node(worker_node);
    const auto & device = graph.node(compute_node);
    if (!main.is_cpu || !worker.is_cpu || device.is_cpu || main.lane_id == worker.lane_id || main.gpu_id != worker.gpu_id || main.gpu_id != device.gpu_id)
        throw std::invalid_argument("Async device operation needs separate same-rank main/worker and device resources");
    const auto main_lane = std::string(graph.node_lane_key(main_node));
    const auto name = "target " + std::string(operation);
    GeneratedDeviceOperation result;
    auto & plan = result.plan;
    std::optional<size_t> previous;
    result.submit = plan.append_main(main_lane, name + " prepare/submit", previous);
    const auto submitted = plan.append_main(main_lane, name + " queued", previous, "");
    const auto task = append_submitted_device(plan, graph, worker_node, submitted, graph.node_lane_key(compute_node), name, " device");
    result.worker = task.worker;
    result.device = task.device;
    plan.host_return = plan.append_main(main_lane, name + " host return", previous, "");
    result.worker_return = task.returned;
    plan.edges.push_back({ task.worker, task.device, core::DagEdgeKind::Mutation });
    plan.streams.push_back({ compute_node, task.device, task.device });
    return result;
}

} // namespace markov::trace_graph::modules::hicache::runtime
