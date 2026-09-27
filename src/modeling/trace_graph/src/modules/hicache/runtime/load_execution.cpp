#include "markov/trace_graph/modules/hicache/runtime/load_execution.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::runtime {
namespace {

size_t append_main_work(HiCacheHostExpansion & plan, std::string_view lane, std::string name, const LoadIndexOperationCost & cost,
                        std::optional<size_t> & previous, std::string category = "hicache_patch") {
    const auto node = plan.nodes.size();
    plan.nodes.push_back({
        { .name = std::move(name),
         .category = std::move(category),
         .lane_key = std::string(lane),
         .duration = cost.main_us,
         .cpu_gap_after = cost.main_residual_us }
    });
    if (previous) plan.edges.push_back({ *previous, node, core::DagEdgeKind::Sequential });
    previous = node;
    return node;
}

struct SubmittedDevice {
    size_t worker, device;
};

// The queued main-thread boundary releases a worker task; it does not wait
// for the device. The caller connects the returned nodes and stream/event dependencies.
SubmittedDevice append_submitted_device(HiCacheHostExpansion & plan, const core::DagGraph & graph, size_t worker, size_t submitted, std::string_view lane,
                                        const LoadIndexOperationCost & cost, std::string name, std::string device_suffix = "") {
    const auto job = plan.nodes.size();
    plan.nodes.push_back({
        { .name = name + " worker",
         .category = "hicache_patch",
         .lane_key = std::string(graph.node_lane_key(worker)),
         .duration = cost.worker_us,
         .cpu_task_ready_delay_us = cost.dispatch_us },
        submitted,
        worker
    });
    const auto device = plan.nodes.size();
    plan.nodes.push_back({
        { .name = std::move(name) + device_suffix, .category = "hicache_patch", .is_cpu = false, .lane_key = std::string(lane), .duration = cost.device_us }
    });
    return { job, device };
}

} // namespace

HiCacheHostExpansion generate_layer_wait(const core::DagGraph & graph, size_t main, size_t worker, size_t compute, const LoadIndexOperationCost & cost,
                                         uint64_t after_us, uint64_t after_residual_us) {
    auto plan = generate_load_index_operation(graph, main, worker, compute, cost, "layer event wait");
    // Only the device stream waits. Its event is the target layer completion,
    // supplied at execution; the CPU returns after submitting the wait.
    plan.event_waits.push_back({ core::DagNode::kNoNode, plan.streams.front().last });
    const auto tail = plan.host_return;
    plan.nodes[tail].work.duration = after_us;
    plan.nodes[tail].work.cpu_gap_after = after_residual_us;
    plan.host_return = plan.nodes.size();
    plan.nodes.push_back({
        { .name = "target layer wait host return", .lane_key = std::string(graph.node_lane_key(main)) }
    });
    plan.edges.push_back({ tail, plan.host_return, core::DagEdgeKind::Sequential });
    return plan;
}

GeneratedLoadSubmission generate_load_submission(const core::DagGraph & graph, size_t main, size_t worker, size_t compute, size_t load_stream, size_t pages,
                                                 uint64_t page_bytes, size_t layers, const LoadSubmissionCost & cost) {
    if (!pages || !layers || !page_bytes || page_bytes % 2 || graph.node(compute).lane_id == graph.node(load_stream).lane_id || graph.node(load_stream).is_cpu
        || graph.node(main).gpu_id != graph.node(load_stream).gpu_id || !graph.node(main).is_cpu || !graph.node(worker).is_cpu || graph.node(compute).is_cpu
        || graph.node(main).lane_id == graph.node(worker).lane_id || graph.node(main).gpu_id != graph.node(worker).gpu_id
        || graph.node(main).gpu_id != graph.node(compute).gpu_id)
        throw std::invalid_argument("Generated Ascend load needs whole K/V pages, layers and a separate load stream");
    GeneratedLoadSubmission result;
    auto & plan = result.plan;
    const auto main_lane = std::string(graph.node_lane_key(main));
    const auto compute_lane = std::string(graph.node_lane_key(compute));
    const auto load_lane = std::string(graph.node_lane_key(load_stream));
    std::optional<size_t> previous_main, previous_device;
    const auto cpu = [&](const LoadIndexOperationCost & c, const std::string & name) { return append_main_work(plan, main_lane, name, c, previous_main); };
    const auto event = [&](const LoadIndexOperationCost & c, const std::string & name, const std::string & lane) {
        (void)cpu(c, name + " submit");
        const auto submitted = cpu({}, name + " queued");
        const auto task = append_submitted_device(plan, graph, worker, submitted, lane, c, name);
        plan.edges.push_back({ task.worker, task.device, core::DagEdgeKind::Mutation });
        return task.device;
    };
    (void)cpu(cost.before_sync, "target load before index synchronization");
    const auto sync = cpu({}, "target load index synchronization");
    plan.waits.push_back({ compute, sync });
    const auto ready = event(cost.start_record, "target load compute ready", compute_lane);
    plan.streams.push_back({ compute, ready, ready });
    const auto wait = event(cost.wait_event, "target load wait for compute", load_lane);
    plan.edges.push_back({ ready, wait, core::DagEdgeKind::Sync });
    previous_device = wait;
    plan.write_start = wait;
    for (size_t page = 0; page < pages; ++page) {
        for (size_t kv = 0; kv < 2; ++kv) {
            const auto & c = page == 0 && kv == 0 ? cost.first_copy : cost.copy;
            (void)cpu(c, "target load K/V page submission");
            const auto submit = cpu({}, "target load K/V page submitted");
            const auto transfer = plan.nodes.size();
            plan.nodes.push_back({
                { .name = "target load K/V page transfer", .category = "hicache_patch", .is_cpu = false, .lane_key = load_lane }
            });
            plan.edges.push_back({ submit, transfer, core::DagEdgeKind::Mutation });
            plan.edges.push_back({ *previous_device, transfer, core::DagEdgeKind::Stream });
            plan.payload.emplace(transfer, page_bytes / 2);
            plan.payload_bytes = core::checked_add_u64(plan.payload_bytes, page_bytes / 2, "Load payload overflow");
            previous_device = transfer;
        }
    }
    for (size_t layer = 0; layer < layers; ++layer) {
        const auto record = event(layer == 0 ? cost.first_layer_record : cost.layer_record, "target load layer complete", load_lane);
        plan.edges.push_back({ *previous_device, record, core::DagEdgeKind::Stream });
        previous_device = record;
        result.layer_records.push_back(record);
    }
    plan.completion = *previous_device;
    plan.streams.push_back({ load_stream, wait, plan.completion });
    (void)cpu(cost.tail, "target load bookkeeping");
    plan.host_return = cpu({}, "target load host return");
    return result;
}


HiCacheHostExpansion generate_load_index_operation(const core::DagGraph & graph, size_t main_node, size_t worker_node, size_t compute_node,
                                                   const LoadIndexOperationCost & cost, std::string_view operation) {
    const auto & main = graph.node(main_node);
    const auto & worker = graph.node(worker_node);
    const auto & device = graph.node(compute_node);
    if (!main.is_cpu || !worker.is_cpu || device.is_cpu || main.lane_id == worker.lane_id || main.gpu_id != worker.gpu_id || main.gpu_id != device.gpu_id)
        throw std::invalid_argument("Load index operation needs separate same-rank main/worker and device resources");
    const auto main_lane = std::string(graph.node_lane_key(main_node));
    const auto name = "target load index " + std::string(operation);
    HiCacheHostExpansion plan;
    std::optional<size_t> previous;
    (void)append_main_work(plan, main_lane, name + " prepare/submit", cost, previous);
    const auto submitted = append_main_work(plan, main_lane, name + " queued", {}, previous, "");
    const auto task = append_submitted_device(plan, graph, worker_node, submitted, graph.node_lane_key(compute_node), cost, name, " device");
    plan.host_return = append_main_work(plan, main_lane, name + " host return", {}, previous, "");
    plan.edges.push_back({ task.worker, task.device, core::DagEdgeKind::Mutation });
    plan.streams.push_back({ compute_node, task.device, task.device });
    return plan;
}

std::vector<size_t> generate_load_layer_transfers(simulation::FutureDag & future, size_t ready, std::string_view lane, std::span<const uint64_t> layer_bytes,
                                                  uint64_t service_us, std::span<const size_t> completion_records) {
    if (lane.empty() || layer_bytes.empty() || (!completion_records.empty() && completion_records.size() != layer_bytes.size()))
        throw std::invalid_argument("Load transfer needs a resource and one completion per target layer");
    uint64_t total = 0;
    for (const auto bytes : layer_bytes) { total = core::checked_add_u64(total, bytes, "Load layer byte count overflow"); }
    if (!total) throw std::invalid_argument("Load service distribution requires a positive total byte weight");
    std::vector<size_t> records;
    uint64_t prefix = 0, assigned = 0;
    auto previous = ready;
    for (size_t i = 0; i < layer_bytes.size(); ++i) {
        prefix += layer_bytes[i];
        const auto cumulative = core::floor_multiply_divide_u64(service_us, prefix, total).value();
        const auto transfer = future.append({ .name = "target H2D layer transfer",
                                              .category = "hicache_patch",
                                              .is_cpu = false,
                                              .lane_key = std::string(lane),
                                              .duration = cumulative - assigned });
        const auto record =
            completion_records.empty()
                ? future.append({ .name = "target H2D layer complete", .category = "hicache_patch", .is_cpu = false, .lane_key = std::string(lane) })
                : completion_records[i];
        future.depend(previous, transfer);
        future.depend(transfer, record);
        records.push_back(record);
        previous = record;
        assigned = cumulative;
    }
    return records;
}

} // namespace markov::trace_graph::modules::hicache::runtime
