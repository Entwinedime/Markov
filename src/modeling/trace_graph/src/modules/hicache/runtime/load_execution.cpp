#include "markov/trace_graph/modules/hicache/runtime/load_execution.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include <array>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::runtime {
namespace {

struct SubmittedDevice {
    size_t worker, device, returned;
};

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

GeneratedPagedRelease generate_paged_release(const core::DagGraph & graph, size_t main, size_t worker, size_t compute, bool paged) {
    GeneratedPagedRelease result;
    auto & plan = result.plan;
    std::optional<size_t> previous;
    const auto lane = graph.node_lane_key(main);
    const auto cpu = [&](ReleaseOperation role, const char * name) {
        const auto node = plan.append_main(lane, name, previous);
        result.operations.push_back({ role, { .main = node } });
        return node;
    };
    if (paged) {
        (void)cpu(ReleaseOperation::BeforeFirstSync, "target release index preparation");
        const auto first_sync = cpu(ReleaseOperation::FirstSync, "target release index readback");
        plan.waits.push_back({ compute, first_sync });

        (void)cpu(ReleaseOperation::BeforeSecondSync, "target release unique pages");
        const auto second_sync = cpu(ReleaseOperation::SecondSync, "target release unique-page readback");
        plan.waits.push_back({ compute, second_sync });
    }

    (void)cpu(ReleaseOperation::Concatenate, "target release concatenate submit");
    const auto queued = plan.append_main(lane, "target release concatenate queued", previous);
    const auto task = append_submitted_device(plan, graph, worker, queued, graph.node_lane_key(compute), "target release concatenate");
    result.operations.back().second.worker = task.worker;
    result.operations.back().second.device = task.device;
    result.operations.back().second.worker_return = task.returned;
    plan.edges.push_back({ task.worker, task.device, core::DagEdgeKind::Mutation });
    plan.streams.push_back({ compute, task.device, task.device });

    (void)cpu(ReleaseOperation::Tail, "target release bookkeeping");
    plan.host_return = plan.append_main(lane, "target release host return", previous);
    return result;
}

void bind_paged_release(GeneratedPagedRelease & generated, const cost::PagedReleaseCost & cost) {
    // Indexed by the structural role, not by a model or observed duration.
    const std::array values{
        &cost.before_first_sync, &cost.first_sync, &cost.before_second_sync, &cost.second_sync, &cost.concatenate, &cost.tail,
    };
    for (const auto & [operation, slots] : generated.operations) bind_host_operation(generated.plan, slots, *values[static_cast<size_t>(operation)]);
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
                                                           size_t pages, uint64_t page_bytes, size_t completion_count, std::string_view operation) {
    if (!pages || !completion_count || !page_bytes || page_bytes % 2 || graph.node(compute).lane_id == graph.node(transfer_stream).lane_id
        || graph.node(transfer_stream).is_cpu || graph.node(main).gpu_id != graph.node(transfer_stream).gpu_id || !graph.node(main).is_cpu
        || !graph.node(worker).is_cpu || graph.node(compute).is_cpu || graph.node(main).lane_id == graph.node(worker).lane_id
        || graph.node(main).gpu_id != graph.node(worker).gpu_id || graph.node(main).gpu_id != graph.node(compute).gpu_id)
        throw std::invalid_argument("Generated Ascend transfer needs whole K/V pages, completions and a separate transfer stream");
    GeneratedTransferSubmission result;
    auto & plan = result.plan;
    const auto main_lane = std::string(graph.node_lane_key(main));
    const auto compute_lane = std::string(graph.node_lane_key(compute));
    const auto transfer_lane = std::string(graph.node_lane_key(transfer_stream));
    const auto prefix = "target " + std::string(operation);
    std::optional<size_t> previous_main, previous_device;
    const auto cpu = [&](const std::string & name) { return plan.append_main(main_lane, name, previous_main); };
    const auto operation_cpu = [&](TransferOperation role, const std::string & name) {
        const auto node = cpu(name);
        result.operations.push_back({ role, { .main = node } });
        return node;
    };
    const auto event = [&](TransferOperation role, const std::string & name, const std::string & lane) {
        (void)operation_cpu(role, name + " submit");
        const auto submitted = cpu(name + " queued");
        const auto task = append_submitted_device(plan, graph, worker, submitted, lane, name);
        result.operations.back().second.worker = task.worker;
        result.operations.back().second.device = task.device;
        result.operations.back().second.worker_return = task.returned;
        plan.edges.push_back({ task.worker, task.device, core::DagEdgeKind::Mutation });
        return task.device;
    };
    (void)operation_cpu(TransferOperation::PrepareSync, prefix + " before index synchronization");
    const auto sync = operation_cpu(TransferOperation::IndexSync, prefix + " index synchronization");
    plan.waits.push_back({ compute, sync });
    const auto ready = event(TransferOperation::StartRecord, prefix + " compute ready", compute_lane);
    plan.streams.push_back({ compute, ready, ready });
    const auto wait = event(TransferOperation::WaitEvent, prefix + " wait for compute", transfer_lane);
    plan.edges.push_back({ ready, wait, core::DagEdgeKind::Sync });
    previous_device = wait;
    plan.write_start = wait;
    for (size_t page = 0; page < pages; ++page) {
        for (size_t kv = 0; kv < 2; ++kv) {
            const auto role = page == 0 && kv == 0 ? TransferOperation::FirstCopy : TransferOperation::Copy;
            (void)operation_cpu(role, prefix + " K/V page submission");
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
    for (size_t record_index = 0; record_index < completion_count; ++record_index) {
        const auto role = record_index == 0 ? TransferOperation::FirstCompletionRecord : TransferOperation::CompletionRecord;
        const auto record = event(role, prefix + " transfer complete", transfer_lane);
        plan.edges.push_back({ *previous_device, record, core::DagEdgeKind::Stream });
        previous_device = record;
        result.completion_records.push_back(record);
    }
    plan.completion = *previous_device;
    plan.streams.push_back({ transfer_stream, wait, plan.completion });
    (void)operation_cpu(TransferOperation::Tail, prefix + " bookkeeping");
    plan.host_return = cpu(prefix + " host return");
    return result;
}

void bind_page_first_submission(GeneratedTransferSubmission & generated, const cost::PageFirstSubmissionCost & cost) {
    const std::array values{
        &cost.prepare_sync,      &cost.index_sync, &cost.start_record, &cost.wait_event, &cost.first_copy, &cost.copy, &cost.first_completion_record,
        &cost.completion_record, &cost.tail,
    };
    for (const auto & [operation, slots] : generated.operations) bind_host_operation(generated.plan, slots, *values[static_cast<size_t>(operation)]);
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
