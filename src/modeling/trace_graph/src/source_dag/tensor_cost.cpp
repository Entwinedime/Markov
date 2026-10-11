#include "markov/trace_graph/source_dag/tensor_cost.hpp"
#include "markov/trace_graph/core/dag_builder.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/source_dag/observations.hpp"

#include <algorithm>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace markov::trace_graph::source_dag {

std::optional<double> observe_device_event_wait_service(const modules::hicache::patch::HiCacheSourceDagIndex & source, size_t node) {
    const auto & graph = source.graph();
    const auto & wait = graph.node(node);
    const auto & event = graph.event_for_node(node);
    if (wait.is_cpu || wait.kind != core::DagNodeKind::TraceEvent || event.name != "EVENT_WAIT") return std::nullopt;

    std::optional<size_t> producer;
    for (const auto edge_id : source.incoming_edge_ids(node)) {
        const auto & edge = graph.edge(edge_id);
        if (!edge.active || edge.kind != core::DagEdgeKind::Sync) continue;
        if (producer) return std::nullopt;
        producer = edge.src;
    }
    if (!producer) return std::nullopt;
    const auto & recorded = graph.node(*producer);
    if (recorded.is_cpu || recorded.kind != core::DagNodeKind::TraceEvent) return std::nullopt;

    const auto & completion = graph.event_for_node(*producer);
    const auto end_us = core::checked_add_u64(completion.ts, completion.dur, "Event record completion overflow");
    const auto end_ns = core::checked_add_u64(core::checked_multiply_u64(end_us, 1'000, "Event record completion overflow"),
                                              static_cast<uint64_t>(completion.ts_submicro_ns) + completion.dur_submicro_ns,
                                              "Event record completion overflow");
    const auto begin_ns = core::checked_add_u64(core::checked_multiply_u64(event.ts, 1'000, "Event wait timestamp overflow"),
                                                event.ts_submicro_ns,
                                                "Event wait timestamp overflow");
    if (end_ns > begin_ns) return std::nullopt;

    return static_cast<double>(event.dur) + static_cast<double>(event.dur_submicro_ns) / 1000.0;
}

std::optional<uint64_t> observe_nonblocking_cpu_service(const modules::hicache::patch::HiCacheSourceDagIndex & source, size_t node) {
    const auto & graph = source.graph();
    const auto & event = graph.event_for_node(node);
    if (graph.node(node).kind != core::DagNodeKind::TraceEvent) return std::nullopt;

    std::vector<size_t> waits;
    for (const auto edge_id : source.incoming_edge_ids(node)) {
        const auto & edge = graph.edge(edge_id);
        if (edge.active && edge.kind == core::DagEdgeKind::Sync) waits.push_back(edge.src);
    }
    const bool recorded_sync = core::synchronizes_recorded_event(event);
    const bool frontier_sync = core::synchronizes_device_frontier(event);
    if (waits.empty() && !recorded_sync && !frontier_sync) {
        if (graph.node(node).duration != event.dur) return std::nullopt;
        return graph.cpu_service_node_duration(node);
    }

    // Only a known single-resource synchronization can prove zero device
    // waiting from one predecessor. An incomplete device-wide frontier is
    // not such a proof, nor is an unbound synchronization API.
    const bool stream_sync = event.name == "AscendCL@aclrtSynchronizeStream" || event.name == "AscendCL@aclrtSynchronizeStreamWithTimeout";
    if ((!recorded_sync && !stream_sync) || waits.size() != 1) return std::nullopt;
    const auto producer = waits.front();
    if (graph.node(producer).is_cpu || graph.node(producer).kind != core::DagNodeKind::TraceEvent) return std::nullopt;
    const auto & completed = graph.event_for_node(producer);
    const auto begin_ns = core::checked_add_u64(core::checked_multiply_u64(event.ts, 1'000, "CPU observation timestamp overflow"),
                                                event.ts_submicro_ns,
                                                "CPU observation timestamp overflow");
    const auto completion_us = core::checked_add_u64(completed.ts, completed.dur, "Device completion timestamp overflow");
    const auto completion_ns = core::checked_add_u64(core::checked_multiply_u64(completion_us, 1'000, "Device completion timestamp overflow"),
                                                     static_cast<uint64_t>(completed.ts_submicro_ns) + completed.dur_submicro_ns,
                                                     "Device completion timestamp overflow");
    if (completion_ns > begin_ns) return std::nullopt;

    // Use the preserved trace interval, never the graph's fixed sync-return
    // value. Corrected and profiled observations retain distinct scopes.
    return graph.cpu_service_cost().duration({ event.pid, event.tid }, event.ts, core::checked_add_u64(event.ts, event.dur, "CPU observation end overflow"));
}

namespace {

std::optional<uint64_t> readback_ready_us(const core::DagGraph & graph, const SynchronousReadback & readback) {
    const auto & wait = graph.event_for_node(readback.synchronization);
    const auto & device = graph.event_for_node(readback.device_completion);
    const auto end_ns = [](const core::TraceEvent & event) {
        const auto end_us = core::checked_add_u64(event.ts, event.dur, "Readback end overflow");
        return core::checked_add_u64(core::checked_multiply_u64(end_us, 1'000, "Readback end timestamp overflow"),
                                     static_cast<uint64_t>(event.ts_submicro_ns) + event.dur_submicro_ns,
                                     "Readback end timestamp overflow");
    };
    const auto start_ns = core::checked_add_u64(core::checked_multiply_u64(wait.ts, 1'000, "Readback start timestamp overflow"),
                                                wait.ts_submicro_ns,
                                                "Readback start timestamp overflow");
    const auto ready_ns = std::max(start_ns, end_ns(device));
    // The dependency establishes a readback even when profiler clocks disagree.
    // Such a sample cannot establish its non-waiting return cost.
    if (ready_ns > end_ns(wait)) return std::nullopt;
    return ready_ns / 1'000 + static_cast<uint64_t>(ready_ns % 1'000 != 0);
}

// The caller establishes serial ordering separately. This function only
// determines whether every member retains a raw device-service measurement.
std::optional<double> measured_device_service(const modules::hicache::patch::HiCacheSourceDagIndex & source, const std::set<size_t> & devices) {
    const auto & graph = source.graph();
    double duration = 0;
    for (const auto node : devices) {
        const auto & value = graph.node(node);
        const auto & event = graph.event_for_node(node);
        // Stream order proves serial work, not that normalization preserved
        // measured service. Generated/rewritten durations are not samples.
        if (value.kind != core::DagNodeKind::TraceEvent || value.duration != event.dur) return std::nullopt;
        duration += static_cast<double>(event.dur) + static_cast<double>(event.dur_submicro_ns) / 1000.0;
    }
    // Scheduling gaps between kernels do not become device service.
    return duration;
}

std::optional<uint64_t> blocking_return_begin(const modules::hicache::patch::HiCacheSourceDagIndex & source, size_t device_completion,
                                              const std::vector<size_t> & caller_nodes, uint64_t submitted, uint64_t end) {
    const auto & graph = source.graph();
    bool synchronized = false;
    auto ready = submitted;
    for (const auto node : caller_nodes) {
        const auto & event = graph.event_for_node(node);
        const bool known_sync = core::synchronizes_recorded_event(event) || event.name == "AscendCL@aclrtSynchronizeStream"
                                || event.name == "AscendCL@aclrtSynchronizeStreamWithTimeout";
        std::vector<size_t> waits;
        for (const auto edge_id : source.incoming_edge_ids(node)) {
            const auto & edge = graph.edge(edge_id);
            if (edge.kind == core::DagEdgeKind::Sync) waits.push_back(edge.src);
        }
        for (const auto edge_id : source.outgoing_edge_ids(node))
            if (graph.edge(edge_id).kind == core::DagEdgeKind::Correlation) return std::nullopt;

        if (waits.empty() && !known_sync && !core::synchronizes_device_frontier(event)) continue;
        if (!known_sync || waits.size() != 1 || waits.front() != device_completion || synchronized) return std::nullopt;
        const auto & producer = graph.node(waits.front());
        if (producer.kind != core::DagNodeKind::TraceEvent || producer.is_cpu) return std::nullopt;
        const auto & completion = graph.event_for_node(producer.id);
        const auto completed_us = core::checked_add_u64(completion.ts, completion.dur, "Blocking completion overflow");
        const auto completed_ns = core::checked_add_u64(core::checked_multiply_u64(completed_us, 1'000, "Blocking completion overflow"),
                                                        static_cast<uint64_t>(completion.ts_submicro_ns) + completion.dur_submicro_ns,
                                                        "Blocking completion overflow");
        const auto returned_us = core::checked_add_u64(event.ts, event.dur, "Blocking sync return overflow");
        const auto returned_ns = core::checked_add_u64(core::checked_multiply_u64(returned_us, 1'000, "Blocking sync return overflow"),
                                                       static_cast<uint64_t>(event.ts_submicro_ns) + event.dur_submicro_ns,
                                                       "Blocking sync return overflow");
        if (completed_ns > returned_ns) return std::nullopt;

        // Round the device completion up: never include its fractional last
        // microsecond in the CPU-side post-completion interval.
        ready = std::max(ready, completed_ns / 1'000 + static_cast<uint64_t>(completed_ns % 1'000 != 0));
        synchronized = true;
    }
    return synchronized && ready <= end ? std::optional{ ready } : std::nullopt;
}

struct LaunchMeasurement {
    std::optional<uint64_t> worker_us, residual_us;
    std::optional<double> device_us;
    std::string device_name;
};

// Framework input metadata establishes dtype/layout. Native task labels
// additionally corroborate operator family, order and execution engine.
bool matches_native_launch(const structure::OperationWork & work, const LaunchMeasurement & measured) {
    const auto expected = native_launch_label(work.implementation);
    return expected && measured.device_name.ends_with(*expected);
}

struct TaskMeasurement {
    std::vector<LaunchMeasurement> launches;
    std::optional<uint64_t> return_us, return_residual_us;
};

std::optional<TaskMeasurement> observe_worker_launch_sequence(const modules::hicache::patch::HiCacheSourceDagIndex & source,
                                                              const simulation::detail::CpuTaskQueues & queues, std::vector<size_t> workers,
                                                              const std::set<size_t> & devices) {
    if (workers.empty()) return std::nullopt;
    const auto & graph = source.graph();
    std::ranges::sort(workers, {}, [&](size_t node) { return std::pair{ graph.event_for_node(node).ts, node }; });
    const auto task_id = queues.node_task.at(workers.front());
    if (task_id == core::DagNode::kNoNode) return std::nullopt;
    const auto & task = queues.tasks.at(task_id);
    if (task.first != workers.front() || task.last != workers.back()) return std::nullopt;

    std::map<size_t, std::set<size_t>> launches;
    std::set<size_t> launched_devices;
    const std::set<size_t> members(workers.begin(), workers.end());
    for (const auto node : workers) {
        if (queues.node_task.at(node) != task_id) return std::nullopt;
        for (const auto edge_id : source.outgoing_edge_ids(node)) {
            const auto & edge = graph.edge(edge_id);
            if (edge.active && edge.kind == core::DagEdgeKind::Sequential && graph.node(edge.dst).active && queues.node_task.at(edge.dst) == task_id
                && !members.contains(edge.dst))
                return std::nullopt;
            if (!edge.active || edge.kind != core::DagEdgeKind::Correlation || graph.node(edge.dst).is_cpu) continue;
            // All device work launched by this task must belong to the call.
            // Otherwise an omitted launch would be mislabeled as CPU return
            // work and a multi-launch task could pass as a single-launch one.
            if (!devices.contains(edge.dst)) return std::nullopt;
            launches[node].insert(edge.dst);
            if (!launched_devices.insert(edge.dst).second) return std::nullopt;
        }
    }
    if (launches.empty() || launched_devices != devices) return std::nullopt;

    TaskMeasurement result;
    std::optional<size_t> previous_device;
    uint64_t service = 0, residual = 0;
    bool cpu_measurable = true;
    uint64_t previous_end = graph.event_for_node(workers.front()).ts;
    for (const auto node : workers) {
        const auto & event = graph.event_for_node(node);
        if (event.ts < previous_end) return std::nullopt;
        const auto measured = observe_nonblocking_cpu_service(source, node);
        // Device service has its own correlation/stream proof. An unknown
        // CPU wait invalidates this CPU segment, not the device measurement
        // or later independently measurable worker segments.
        if (measured) service = core::checked_add_u64(service, *measured, "Worker service overflow");
        else cpu_measurable = false;
        residual =
            core::checked_add_u64(residual, graph.cpu_service_cost().duration({ event.pid, event.tid }, previous_end, event.ts), "Worker residual overflow");
        previous_end = core::checked_add_u64(event.ts, event.dur, "Worker observation end overflow");
        if (const auto found = launches.find(node); found != launches.end()) {
            const auto chain = observe_serial_device_chain(source, found->second);
            // A stream-event wait can be submitted by a nonblocking worker.
            // Its CPU segments remain measurable, but the waiting device
            // interval is not a service sample. Multiple launches additionally
            // need a proven serial device chain to identify their stage order.
            if (!chain && launches.size() != 1) return std::nullopt;
            if (previous_device && !std::ranges::any_of(source.incoming_edge_ids(chain->first), [&](size_t edge_id) {
                    const auto & edge = graph.edge(edge_id);
                    return edge.active && edge.kind == core::DagEdgeKind::Stream && edge.src == *previous_device;
                }))
                return std::nullopt;
            std::optional<double> device_us;
            if (chain) {
                device_us = measured_device_service(source, found->second);
                previous_device = chain->last;
            }
            result.launches.push_back({ cpu_measurable ? std::optional{ service } : std::nullopt,
                                        cpu_measurable ? std::optional{ residual } : std::nullopt,
                                        device_us,
                                        found->second.size() == 1 ? graph.event_for_node(*found->second.begin()).name : std::string{} });
            service = 0;
            residual = 0;
            cpu_measurable = true;
        }
    }
    if (cpu_measurable) {
        result.return_us = service;
        result.return_residual_us = residual;
    }
    return result;
}
} // namespace

std::optional<WorkerLaunchObservation> observe_worker_launch(const modules::hicache::patch::HiCacheSourceDagIndex & source,
                                                             const simulation::detail::CpuTaskQueues & queues, std::vector<size_t> workers,
                                                             const std::set<size_t> & devices) {
    const auto task = observe_worker_launch_sequence(source, queues, std::move(workers), devices);
    if (!task || task->launches.size() != 1) return std::nullopt;
    const auto & launch = task->launches.front();
    if (!launch.worker_us || !task->return_us) return std::nullopt;
    return WorkerLaunchObservation{ *launch.worker_us, *launch.residual_us, *task->return_us, *task->return_residual_us };
}

nlohmann::json queued_task_observations(const modules::hicache::patch::HiCacheSourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues,
                                        const QueuedCallTask & observed, const structure::OperationWork & work, bool cpu_service_applied) {
    const auto & graph = source.graph();
    const auto & task = queues.tasks.at(observed.task);
    auto rows = nlohmann::json::array();
    const auto position = measurement_position(graph.event_for_node(task.submission));
    if (!position) return rows;

    const auto emit = [&](const char * scope, uint64_t duration) {
        rows.push_back({
            {       "position",                                                  "queued_task:" + *position },
            {      "operation",                                                              work.operation },
            { "implementation",                                                         work.implementation },
            {       "features",                                                               work.features },
            {    "duration_us",                                                                    duration },
            {   "timing_scope", cpu_service_applied ? std::string(scope) : "profiled_" + std::string(scope) }
        });
    };
    emit("dispatch_interval", graph.cpu_service_ready_delay(task.first, task.ready_delay_us));

    if (!observed.devices.empty()) {
        const auto measured = observe_worker_launch_sequence(source, queues, observed.workers, observed.devices);
        if (!measured || measured->launches.size() != 1) return rows;
        const auto & launch = measured->launches.front();
        if (launch.worker_us) {
            emit("worker_launch_service", *launch.worker_us);
            emit("worker_launch_residual_interval", *launch.residual_us);
        }
        if (measured->return_us) {
            emit("worker_return_service", *measured->return_us);
            emit("worker_return_residual_interval", *measured->return_residual_us);
        }
        return rows;
    }

    uint64_t service = 0, residual = 0;
    auto previous_end = graph.event_for_node(task.first).ts;
    for (const auto node : observed.workers) {
        const auto & event = graph.event_for_node(node);
        const auto measured = observe_nonblocking_cpu_service(source, node);
        if (!measured || event.ts < previous_end) return rows;
        service = core::checked_add_u64(service, *measured, "Queued CPU service overflow");
        residual = core::checked_add_u64(residual,
                                         graph.cpu_service_cost().duration({ event.pid, event.tid }, previous_end, event.ts),
                                         "Queued CPU residual overflow");
        previous_end = core::checked_add_u64(event.ts, event.dur, "Queued CPU task end overflow");
    }
    emit("worker_service", service);
    emit("worker_residual_interval", residual);
    return rows;
}

bool append_cpu_interval_observations(const modules::hicache::patch::HiCacheSourceDagIndex & source, const core::TraceEvent & outer,
                                      const structure::OperationWork & work, uint64_t begin, uint64_t end, bool corrected, nlohmann::json & rows,
                                      std::optional<size_t> permitted_submission) {
    const auto & graph = source.graph();
    const auto position = measurement_position(outer);
    if (!position) return false;

    if (end < begin) return false;
    const auto part = source.call_interval_ownership(outer.pid, outer.tid, begin, end - begin);
    if ((part.status != "ready" && part.status != "zero_duration") || part.has_node_overlap) return false;

    double service = 0, residual = 0;
    for (const auto node : part.owned_node_ids) {
        // Unmodeled submissions cannot become CPU preparation. Synchronizing
        // APIs are admitted below only with proof of zero device waiting.
        for (const auto edge_id : source.outgoing_edge_ids(node)) {
            const auto & edge = graph.edge(edge_id);
            if (edge.active && edge.kind == core::DagEdgeKind::Correlation && graph.node(edge.dst).active && permitted_submission != node) return false;
        }
        const auto measured = observe_nonblocking_cpu_service(source, node);
        if (!measured) return false;
        service += *measured;
    }
    for (const auto & gap : part.owned_gap_slices)
        residual += graph.cpu_service_cost().duration({ outer.pid, outer.tid }, gap.owned_start_us, gap.owned_end_us);
    const auto emit = [&](const char * scope, double duration) {
        rows.push_back({
            {       "position", "operator_outer:" + *position + ":" + std::to_string(begin) + ":" + std::to_string(end) + ":" + work.operation },
            {      "operation",                                                                                                 work.operation },
            { "implementation",                                                                                            work.implementation },
            {       "features",                                                                                                  work.features },
            {    "duration_us",                                                                                                       duration },
            {   "timing_scope",                                                                                                          scope },
        });
    };
    emit(corrected ? "cpu_service" : "profiled_cpu", service);
    emit(corrected ? "residual_interval" : "profiled_residual_interval", residual);
    return true;
}

nlohmann::json tensor_operator_observations(const modules::hicache::patch::HiCacheSourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues,
                                            const structure::OperationWork & work, size_t index, std::set<size_t> & measured_devices, bool cpu_service_applied,
                                            bool include_caller_scopes, std::span<const structure::OperationWork> device_launches) {
    const auto & graph = source.graph();
    nlohmann::json rows = nlohmann::json::array();
    const auto position = measurement_position(graph.runtime_observations().at(index));
    if (!position) return rows;

    const auto region = observe_submitted_call_region(source, queues, index);
    if (region.caller.status != "ready" || region.caller.has_node_overlap) return rows;

    if (work.implementation.starts_with("aten._local_scalar_dense:") || work.implementation == "aten.to.cpu"
        || work.implementation.starts_with("aten.to.cpu:")) {
        if (!include_caller_scopes) return rows;
        const auto readback = observe_synchronous_readback(source, region);
        if (!readback) return rows;
        const auto & envelope = graph.runtime_observations().at(index);
        const auto & sync = graph.event_for_node(readback->synchronization);
        const auto end = core::checked_add_u64(envelope.ts, envelope.dur, "Readback envelope end overflow");
        for (const auto node : region.submitted_nodes)
            if (!graph.node(node).is_cpu && measured_devices.contains(node)) return rows;

        (void)append_cpu_interval_observations(source, envelope, work, envelope.ts, sync.ts, cpu_service_applied, rows);
        const auto emit_return = [&](const char * scope, uint64_t begin) {
            rows.push_back({
                { "position", "tensor_operator:" + *position + ":" + work.operation + ".return" },
                { "operation", work.operation + ".return" },
                { "implementation", work.implementation },
                { "features", work.features },
                { "duration_us", graph.cpu_service_cost().duration({ envelope.pid, envelope.tid }, begin, end) },
                { "timing_scope", scope },
            });
        };
        const auto ready_us = readback_ready_us(graph, *readback);
        if (ready_us && *ready_us <= end) emit_return(cpu_service_applied ? "blocking_readback_interval" : "profiled_blocking_readback_interval", *ready_us);

        // Both endpoints below are host observations. Unlike the full return
        // interval, this segment does not require a device-to-host clock map.
        // It includes the synchronous copy and caller tail, but excludes the
        // synchronization API itself. It is not a substitute for its service.
        const auto sync_return = core::checked_add_u64(sync.ts, sync.dur, "Readback synchronization end overflow");
        if (sync_return <= end) emit_return(cpu_service_applied ? "post_sync_readback_interval" : "profiled_post_sync_readback_interval", sync_return);

        for (const auto node : region.submitted_nodes)
            if (!graph.node(node).is_cpu) measured_devices.insert(node);
        return rows;
    }

    std::set<size_t> devices;
    for (const auto node : region.submitted_nodes)
        if (!graph.node(node).is_cpu) devices.insert(node);
    // Submission ownership applies to the whole call. A serial device chain
    // is needed to aggregate device service, not to measure an independently
    // owned CPU submission. In particular, a collective can submit a device
    // wait while its caller and worker still have measurable CPU segments.
    const auto device_chain = observe_serial_device_chain(source, devices);
    if (devices.empty() || std::ranges::any_of(devices, [&](size_t node) { return measured_devices.contains(node); })) return rows;
    const auto device_service = device_chain ? measured_device_service(source, devices) : std::nullopt;
    measured_devices.insert(devices.begin(), devices.end());

    const auto emit = [&](const std::string & operation, const char * scope, double duration) {
        rows.push_back({
            {       "position", "tensor_operator:" + *position + ":" + operation },
            {      "operation",                                        operation },
            { "implementation",                              work.implementation },
            {       "features",                                    work.features },
            {    "duration_us",                                         duration },
            {   "timing_scope",                                            scope },
        });
    };
    if (device_launches.empty() && device_service) emit(work.operation, "device_service", *device_service);

    std::vector<size_t> workers;
    for (const auto node : region.submitted_nodes)
        if (graph.node(node).is_cpu) workers.push_back(node);
    std::optional<uint64_t> submitted_at;
    if (workers.empty()) {
        std::set<size_t> directly_submitted;
        for (const auto node : region.caller.owned_node_ids) {
            for (const auto edge_id : source.outgoing_edge_ids(node)) {
                const auto & edge = graph.edge(edge_id);
                if (!edge.active || edge.kind != core::DagEdgeKind::Correlation || !devices.contains(edge.dst)) continue;
                directly_submitted.insert(edge.dst);
                const auto & submit = graph.event_for_node(node);
                const auto end = core::checked_add_u64(submit.ts, submit.dur, "Direct tensor submission end overflow");
                submitted_at = std::max(submitted_at.value_or(end), end);
            }
        }
        // No worker is a distinct observed path, not missing worker data.
        // Every device operation must have its own caller submission proof.
        if (directly_submitted != devices) return rows;
    }
    else {
        std::ranges::sort(workers, {}, [&](size_t node) { return std::pair{ graph.event_for_node(node).ts, node }; });
        std::set<size_t> task_ids;
        for (const auto node : workers) {
            const auto task_id = queues.node_task.at(node);
            if (task_id == core::DagNode::kNoNode) return rows;
            task_ids.insert(task_id);
        }
        for (const auto task_id : task_ids) {
            const auto & task = queues.tasks.at(task_id);
            if (std::ranges::find(workers, task.first) == workers.end() || std::ranges::find(workers, task.last) == workers.end()
                || std::ranges::find(region.caller.owned_node_ids, task.submission) == region.caller.owned_node_ids.end())
                return rows;

            const auto & submit = graph.event_for_node(task.submission);
            const auto end = core::checked_add_u64(submit.ts, submit.dur, "tensor submission end overflow");
            submitted_at = std::max(submitted_at.value_or(end), end);
        }

        // A call can enqueue several tasks. Its caller boundary is still
        // measurable, but those tasks cannot become one worker observation:
        // their queue waits and internal launch dependencies are distinct.
        // Likewise an unmeasurable worker must not discard measurable caller
        // service. Each scope establishes its own evidence below.
        if (task_ids.size() == 1) {
            const auto & task = queues.tasks.at(*task_ids.begin());
            const auto worker = workers.front();
            const auto measured = observe_worker_launch_sequence(source, queues, workers, devices);
            const auto expected_launches = device_launches.empty() ? size_t{ 1 } : device_launches.size();
            bool matching_program = measured && measured->launches.size() == expected_launches;
            if (matching_program && !device_launches.empty()) {
                for (size_t launch = 0; launch < expected_launches; ++launch)
                    matching_program &= matches_native_launch(device_launches[launch], measured->launches[launch]);
            }
            if (matching_program) {
                for (size_t launch = 0; launch < expected_launches; ++launch) {
                    const auto & description = device_launches.empty() ? work : device_launches[launch];
                    const auto & sample = measured->launches[launch];
                    const auto emit_launch = [&](const char * scope, uint64_t duration) {
                        rows.push_back({
                            {       "position", "tensor_operator:" + *position + ":" + description.operation },
                            {      "operation",                                        description.operation },
                            { "implementation",                                   description.implementation },
                            {       "features",                                         description.features },
                            {    "duration_us",                                                     duration },
                            {   "timing_scope",                                                        scope }
                        });
                    };
                    if (sample.worker_us) {
                        emit_launch(cpu_service_applied ? "worker_launch_service" : "profiled_worker_launch_service", *sample.worker_us);
                        emit_launch(cpu_service_applied ? "worker_launch_residual_interval" : "profiled_worker_launch_residual_interval", *sample.residual_us);
                    }

                    // Ordinary tensor device service was already emitted;
                    // explicit native stages own separate device samples.
                    if (!device_launches.empty() && sample.device_us) emit_launch("device_service", *sample.device_us);
                }
                if (measured->return_us) {
                    emit(work.operation, cpu_service_applied ? "worker_return_service" : "profiled_worker_return_service", *measured->return_us);
                    emit(work.operation,
                         cpu_service_applied ? "worker_return_residual_interval" : "profiled_worker_return_residual_interval",
                         *measured->return_residual_us);
                }
            }
            emit(work.operation,
                 cpu_service_applied ? "dispatch_interval" : "profiled_dispatch_interval",
                 graph.cpu_service_ready_delay(worker, task.ready_delay_us));
        }
    }
    if (!include_caller_scopes) return rows;

    const auto & envelope = graph.runtime_observations().at(index);
    const auto boundary = submitted_at.value();
    const auto end = core::checked_add_u64(envelope.ts, envelope.dur, "tensor envelope end overflow");
    if (boundary < envelope.ts || boundary > end) return rows;

    const auto before = source.timing_interval_ownership(envelope.pid, envelope.tid, envelope.ts, boundary - envelope.ts);
    const auto after = source.timing_interval_ownership(envelope.pid, envelope.tid, boundary, end - boundary);
    const auto complete = [](const auto & part) { return (part.status == "ready" || part.status == "zero_duration") && !part.has_node_overlap; };
    const auto emit_host = [&](const auto & part, const std::string & operation) {
        if (!complete(part)) return;

        double service = 0, residual = 0;
        for (const auto node : part.owned_node_ids) {
            const auto measured = observe_nonblocking_cpu_service(source, node);
            if (!measured) return;
            service += *measured;
        }
        for (const auto & gap : part.owned_gap_slices)
            residual += graph.cpu_service_cost().duration({ envelope.pid, envelope.tid }, gap.owned_start_us, gap.owned_end_us);
        emit(operation, cpu_service_applied ? "cpu_service" : "profiled_cpu", service);
        emit(operation, cpu_service_applied ? "residual_interval" : "profiled_residual_interval", residual);
    };
    emit_host(before, work.operation);
    emit_host(after, work.operation + ".return");
    if (complete(after) && device_chain) {
        if (const auto ready = blocking_return_begin(source, device_chain->last, after.owned_node_ids, boundary, end)) {
            // This measured wall interval includes host reaction/scheduling
            // after completion, not device waiting or a claim of pure CPU.
            emit(work.operation + ".return",
                 cpu_service_applied ? "blocking_return_interval" : "profiled_blocking_return_interval",
                 graph.cpu_service_cost().duration({ envelope.pid, envelope.tid }, *ready, end));
        }
    }
    return rows;
}

void append_tensor_program_observations(const modules::hicache::patch::HiCacheSourceDagIndex & source,
                                         const simulation::detail::CpuTaskQueues & queues, std::span<const structure::TensorOperation> program,
                                         const std::map<size_t, size_t> & matched, std::set<size_t> & measured_devices, bool cpu_service_applied,
                                         nlohmann::json & rows, bool include_caller_scopes, std::optional<size_t> omitted_caller_operation) {
    for (const auto & [position, observation] : matched) {
        const auto & operation = program[position];
        const bool include_caller = include_caller_scopes && position != omitted_caller_operation;
        for (auto & row : tensor_operator_observations(source, queues, operation.work, observation, measured_devices, cpu_service_applied,
                                                       include_caller, operation.device_launches))
            rows.push_back(std::move(row));
    }
}

nlohmann::json tensor_program_outer_observations(const modules::hicache::patch::HiCacheSourceDagIndex & source,
                                                 const SubmittedCallRegion & region, std::span<const structure::TensorOperation> program,
                                                 const std::map<size_t, size_t> & matched, bool cpu_service_applied,
                                                 std::span<const modules::hicache::HiCacheLayerWaitCall> layer_waits) {
    const auto & graph = source.graph();
    const auto & outer = graph.runtime_observations().at(region.observation);
    nlohmann::json rows = nlohmann::json::array();
    // A program rooted at its own Torch envelope already exports caller
    // scopes. Only an explicit CPU tail establishes additional outer work.
    if (!measurement_position(outer) || program.empty() || program.back().execution != structure::TensorExecutionKind::CpuService) return rows;

    const auto outer_end = core::checked_add_u64(outer.ts, outer.dur, "Tensor program end overflow");
    std::vector<const modules::hicache::HiCacheLayerWaitCall *> nested;
    for (const auto & wait : layer_waits) {
        if (!wait.issue.empty() || !wait.before) continue;
        const auto & event = graph.event_for_node(*wait.before);
        if (event.pid == outer.pid && event.tid == outer.tid && wait.start_ns / 1'000 >= outer.ts && wait.end_ns / 1'000 <= outer_end) nested.push_back(&wait);
    }
    std::ranges::stable_sort(nested, [](const auto * left, const auto * right) {
        return std::pair{ left->start_ns, left->position } < std::pair{ right->start_ns, right->position };
    });

    // Preparation alternates with its consumer, followed by the outer return.
    // Partial matching must not charge an omitted consumer as preparation.
    uint64_t previous = outer.ts;
    size_t layer = 0;
    for (size_t i = 0; i + 1 < program.size(); i += 2) {
        const auto & operation = program[i + 1];
        uint64_t begin, end;
        if (operation.execution == structure::TensorExecutionKind::LayerReady) {
            if (layer == nested.size() || nested[layer]->layer != operation.layer_index) return nlohmann::json::array();
            begin = nested[layer]->start_ns / 1'000;
            end = nested[layer++]->end_ns / 1'000;
        }
        else {
            const auto found = matched.find(i + 1);
            if (found == matched.end()) return nlohmann::json::array();
            const auto & event = graph.runtime_observations().at(found->second);
            begin = event.ts;
            end = core::checked_add_u64(begin, event.dur, "Tensor operator end overflow");
        }
        if (begin < previous || end > outer_end
            || !append_cpu_interval_observations(source, outer, program[i].work, previous, begin, cpu_service_applied, rows))
            return nlohmann::json::array();
        previous = end;
    }

    if (layer != nested.size()
        || !append_cpu_interval_observations(source, outer, program.back().work, previous, outer_end, cpu_service_applied, rows))
        return nlohmann::json::array();
    return rows;
}

} // namespace markov::trace_graph::source_dag
