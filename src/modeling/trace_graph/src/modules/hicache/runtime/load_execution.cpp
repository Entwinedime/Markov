#include "markov/trace_graph/modules/hicache/runtime/load_execution.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include <set>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::runtime {

HiCacheHostExpansion generate_layer_wait(const core::DagGraph & graph, size_t main, size_t worker, size_t compute,
    const LoadIndexOperationCost & cost, uint64_t after_us, uint64_t after_residual_us) {
    auto plan = generate_load_index_operation(graph, main, worker, compute, cost, "layer event wait");
    // Only the device stream waits. Its event is the target layer completion,
    // supplied at execution; the CPU returns after submitting the wait.
    plan.event_waits.push_back({core::DagNode::kNoNode, 3});
    plan.nodes[4].work.duration = after_us;
    plan.nodes[4].work.cpu_gap_after = after_residual_us;
    plan.nodes.push_back({{.name = "target layer wait host return", .lane_key = std::string(graph.node_lane_key(main))}});
    plan.edges.push_back({4, 5, core::DagEdgeKind::Sequential});
    plan.host_return = 5;
    return plan;
}

GeneratedLoadSubmission generate_load_submission(
    const core::DagGraph & graph, size_t main, size_t worker, size_t compute, size_t load_stream,
    size_t pages, uint64_t page_bytes, size_t layers, const LoadSubmissionCost & cost) {
    if (!pages || !layers || !page_bytes || page_bytes % 2
        || graph.node(compute).lane_id == graph.node(load_stream).lane_id
        || graph.node(load_stream).is_cpu || graph.node(main).gpu_id != graph.node(load_stream).gpu_id
        || !graph.node(main).is_cpu || !graph.node(worker).is_cpu || graph.node(compute).is_cpu
        || graph.node(main).lane_id == graph.node(worker).lane_id
        || graph.node(main).gpu_id != graph.node(worker).gpu_id || graph.node(main).gpu_id != graph.node(compute).gpu_id)
        throw std::invalid_argument("Generated Ascend load needs whole K/V pages, layers and a separate load stream");
    GeneratedLoadSubmission result;
    auto & plan = result.plan;
    const auto main_lane = std::string(graph.node_lane_key(main));
    const auto worker_lane = std::string(graph.node_lane_key(worker));
    const auto compute_lane = std::string(graph.node_lane_key(compute));
    const auto load_lane = std::string(graph.node_lane_key(load_stream));
    std::optional<size_t> previous_main, previous_device;
    const auto cpu = [&](const LoadIndexOperationCost & c, const std::string & name) {
        const auto id = plan.nodes.size();
        plan.nodes.push_back({{.name = name, .category = "hicache_patch", .lane_key = main_lane,
            .duration = c.main_us, .cpu_gap_after = c.main_residual_us}});
        if (previous_main) plan.edges.push_back({*previous_main, id, core::DagEdgeKind::Sequential});
        previous_main = id;
        return id;
    };
    const auto event = [&](const LoadIndexOperationCost & c, const std::string & name, const std::string & lane) {
        (void)cpu(c, name + " submit");
        const auto submitted = cpu({}, name + " queued");
        const auto job = plan.nodes.size();
        plan.nodes.push_back({{.name = name + " worker", .category = "hicache_patch", .lane_key = worker_lane,
            .duration = c.worker_us, .cpu_task_ready_delay_us = c.dispatch_us}, submitted, worker});
        const auto device = plan.nodes.size();
        plan.nodes.push_back({{.name = name, .category = "hicache_patch", .is_cpu = false, .lane_key = lane,
            .duration = c.device_us}});
        plan.edges.push_back({job, device, core::DagEdgeKind::Mutation});
        return device;
    };
    (void)cpu(cost.before_sync, "target load before index synchronization");
    const auto sync = cpu({}, "target load index synchronization");
    plan.waits.push_back({compute, sync});
    const auto ready = event(cost.start_record, "target load compute ready", compute_lane);
    plan.streams.push_back({compute, ready, ready});
    const auto wait = event(cost.wait_event, "target load wait for compute", load_lane);
    plan.edges.push_back({ready, wait, core::DagEdgeKind::Sync});
    previous_device = wait;
    plan.write_start = wait;
    for (size_t page = 0; page < pages; ++page) {
        for (size_t kv = 0; kv < 2; ++kv) {
            const auto & c = page == 0 && kv == 0 ? cost.first_copy : cost.copy;
            (void)cpu(c, "target load K/V page submission");
            const auto submit = cpu({}, "target load K/V page submitted");
            const auto transfer = plan.nodes.size();
            plan.nodes.push_back({{.name = "target load K/V page transfer", .category = "hicache_patch",
                .is_cpu = false, .lane_key = load_lane}});
            plan.edges.push_back({submit, transfer, core::DagEdgeKind::Mutation});
            plan.edges.push_back({*previous_device, transfer, core::DagEdgeKind::Stream});
            plan.payload.emplace(transfer, page_bytes / 2);
            plan.payload_bytes = core::checked_add_u64(plan.payload_bytes, page_bytes / 2, "Load payload overflow");
            previous_device = transfer;
        }
    }
    for (size_t layer = 0; layer < layers; ++layer) {
        const auto record = event(layer == 0 ? cost.first_layer_record : cost.layer_record,
                                  "target load layer complete", load_lane);
        plan.edges.push_back({*previous_device, record, core::DagEdgeKind::Stream});
        previous_device = record;
        result.layer_records.push_back(record);
    }
    plan.completion = *previous_device;
    plan.streams.push_back({load_stream, wait, plan.completion});
    (void)cpu(cost.tail, "target load bookkeeping");
    plan.host_return = cpu({}, "target load host return");
    return result;
}

std::optional<LoadIndexResources> observe_load_index_resources(
    const patch::HiCacheSourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues,
    size_t main_node, size_t compute_node) {
    const auto & graph = source.graph();
    if (!graph.node(main_node).is_cpu || graph.node(compute_node).is_cpu
        || graph.node(main_node).gpu_id != graph.node(compute_node).gpu_id)
        throw std::invalid_argument("Load resources require a same-rank main thread and compute stream");
    std::optional<size_t> worker;
    for (size_t task_id = 0; task_id < queues.tasks.size(); ++task_id) {
        const auto & task = queues.tasks[task_id];
        if (!graph.node(task.submission).active || !graph.node(task.first).active
            || graph.node(task.submission).lane_id != graph.node(main_node).lane_id
            || graph.node(task.first).gpu_id != graph.node(main_node).gpu_id
            || !graph.event_for_node(task.submission).name.starts_with("Enqueue@")) continue;
        std::vector<size_t> pending{task.first};
        std::set<size_t> visited;
        for (size_t i = 0; i < pending.size(); ++i) {
            const auto node = pending[i];
            if (!visited.insert(node).second) continue;
            for (const auto id : source.outgoing_edge_ids(node)) {
                const auto & edge = graph.edge(id);
                const auto & next = graph.node(edge.dst);
                if (!edge.active || !next.active) continue;
                if (next.is_cpu) {
                    if (queues.node_task.at(edge.dst) == task_id) pending.push_back(edge.dst);
                    continue;
                }
                if (next.lane_id != graph.node(compute_node).lane_id) continue;
                if (worker && graph.node(*worker).lane_id != graph.node(task.first).lane_id)
                    throw std::runtime_error("Compute submissions identify multiple worker FIFOs");
                worker = task.first;
            }
        }
    }
    if (!worker) return std::nullopt;
    return LoadIndexResources{*worker, compute_node};
}

HiCacheHostExpansion generate_load_index_operation(
    const core::DagGraph & graph, size_t main_node, size_t worker_node, size_t compute_node,
    const LoadIndexOperationCost & cost, std::string_view operation) {
    const auto & main = graph.node(main_node);
    const auto & worker = graph.node(worker_node);
    const auto & device = graph.node(compute_node);
    if (!main.is_cpu || !worker.is_cpu || device.is_cpu || main.lane_id == worker.lane_id
        || main.gpu_id != worker.gpu_id || main.gpu_id != device.gpu_id)
        throw std::invalid_argument("Load index operation needs separate same-rank main/worker and device resources");
    const auto main_lane = std::string(graph.node_lane_key(main_node));
    const auto name = "target load index " + std::string(operation);
    HiCacheHostExpansion plan;
    plan.nodes.push_back({{.name = name + " prepare/submit", .category = "hicache_patch",
        .lane_key = main_lane, .duration = cost.main_us, .cpu_gap_after = cost.main_residual_us}});
    plan.nodes.push_back({{.name = name + " queued", .lane_key = main_lane}});
    plan.nodes.push_back({{.name = name + " worker", .category = "hicache_patch",
        .lane_key = std::string(graph.node_lane_key(worker_node)), .duration = cost.worker_us,
        .cpu_task_ready_delay_us = cost.dispatch_us}, 1, worker_node});
    plan.nodes.push_back({{.name = name + " device", .category = "hicache_patch", .is_cpu = false,
        .lane_key = std::string(graph.node_lane_key(compute_node)), .duration = cost.device_us}});
    plan.nodes.push_back({{.name = name + " host return", .lane_key = main_lane}});
    plan.edges = {{0, 1, core::DagEdgeKind::Sequential}, {1, 4, core::DagEdgeKind::Sequential},
                  {2, 3, core::DagEdgeKind::Mutation}};
    plan.streams.push_back({compute_node, 3, 3});
    plan.host_return = 4;
    return plan;
}

std::vector<size_t> generate_load_layer_transfers(
    simulation::FutureDag & future, size_t ready, std::string_view lane,
    std::span<const uint64_t> layer_bytes, uint64_t service_us,
    std::span<const size_t> completion_records) {
    if (lane.empty() || layer_bytes.empty()
        || (!completion_records.empty() && completion_records.size() != layer_bytes.size()))
        throw std::invalid_argument("Load transfer needs a resource and one completion per target layer");
    uint64_t total = 0;
    for (const auto bytes : layer_bytes) {
        total = core::checked_add_u64(total, bytes, "Load layer byte count overflow");
    }
    if (!total) throw std::invalid_argument("Load service distribution requires a positive total byte weight");
    std::vector<size_t> records;
    uint64_t prefix = 0, assigned = 0;
    auto previous = ready;
    for (size_t i = 0; i < layer_bytes.size(); ++i) {
        prefix += layer_bytes[i];
        const auto cumulative = core::floor_multiply_divide_u64(service_us, prefix, total).value();
        const auto transfer = future.append({.name = "target H2D layer transfer", .category = "hicache_patch",
            .is_cpu = false, .lane_key = std::string(lane), .duration = cumulative - assigned});
        const auto record = completion_records.empty()
            ? future.append({.name = "target H2D layer complete", .category = "hicache_patch",
                .is_cpu = false, .lane_key = std::string(lane)})
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
