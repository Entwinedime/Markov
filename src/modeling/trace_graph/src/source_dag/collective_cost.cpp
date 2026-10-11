#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/source_dag/device_collectives.hpp"
#include "markov/trace_graph/source_dag/observations.hpp"
#include "markov/trace_graph/source_dag/tensor_cost.hpp"
#include "markov/trace_graph/structure/phase_shape.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <utility>

namespace markov::trace_graph::source_dag {
namespace {

std::optional<std::pair<uint64_t, uint64_t>> raw_interval_ns(const core::DagGraph & graph, size_t node) {
    const auto & event = graph.event_for_node(node);
    if (graph.node(node).kind != core::DagNodeKind::TraceEvent || !measurement_position(event)) return std::nullopt;
    const auto begin = core::checked_add_u64(core::checked_multiply_u64(event.ts, 1'000, "collective timestamp overflow"),
                                             event.ts_submicro_ns,
                                             "collective timestamp overflow");
    const auto duration = core::checked_add_u64(core::checked_multiply_u64(event.dur, 1'000, "collective duration overflow"),
                                                event.dur_submicro_ns,
                                                "collective duration overflow");
    return std::pair{ begin, core::checked_add_u64(begin, duration, "collective end overflow") };
}

} // namespace

nlohmann::json collective_operator_observations(const modules::hicache::patch::HiCacheSourceDagIndex & source,
                                                const simulation::detail::CpuTaskQueues & queues,
                                                const modules::hicache::HiCachePhaseObservationAudit & phases, bool cpu_service_applied,
                                                nlohmann::json * collective_diagnostics) {
    const auto & graph = source.graph();
    const auto observed = observe_device_collectives(source, queues, phases, collective_diagnostics);
    std::map<size_t, const DeviceCollectiveCall *> calls;
    for (const auto & call : observed.calls) calls.emplace(call.region.observation, &call);
    auto rows = nlohmann::json::array();

    for (const auto & call : observed.calls) {
        const std::string component = call.position.decode_iteration ? "decode" : "prefill";
        const std::string operation = call.transfer == structure::hicache::CollectiveTaskKind::Gather ? "all_gather" : "all_reduce";
        const structure::OperationWork work{
            component,
            component + "." + operation,
            "sglang.NpuCommunicator." + operation + ":" + call.dtype,
            "collective_completion_interval",
            structure::CollectiveShape{ call.rows, call.width, call.element_bytes, call.members.size() }
            .features()
        };
        // ProcessGroupHCCL surrounds the reduction with separate event
        // tasks. Caller intervals end at each enqueue, not at the last
        // enqueue of the whole call; worker/device work is measured separately.
        if (const auto tasks = observe_queued_call_tasks(source, queues, call.region)) {
            if (const auto caller = observe_collective_caller_intervals(source, queues, call, *tasks)) {
                const auto steps = structure::hicache::ordinary_collective_caller_steps(call.transfer);
                const auto & envelope = graph.runtime_observations().at(call.region.observation);
                for (const auto & interval : *caller) {
                    const auto caller_work = structure::hicache::collective_caller_work(work, steps[interval.step]);
                    (void)append_cpu_interval_observations(source,
                                                           envelope,
                                                           caller_work,
                                                           interval.begin_us,
                                                           interval.end_us,
                                                           cpu_service_applied,
                                                           rows,
                                                           interval.submission);
                }
            }

            for (const auto & task : *tasks) {
                const auto & submit = graph.event_for_node(queues.tasks.at(task.task).submission);
                const auto primitive = collective_task_kind(submit.name);
                if (!primitive) continue;

                const auto task_work = structure::hicache::collective_task_work(work, *primitive);
                for (auto & row : queued_task_observations(source, queues, task, task_work, cpu_service_applied)) rows.push_back(std::move(row));
            }
        }

        // Count one shared response, not one nominally independent sample
        // per rank. The peer list is in validated process-group order.
        if (call.peer_observations.front() != call.region.observation) continue;
        uint64_t first_ready = std::numeric_limits<uint64_t>::max(), last_ready = 0, last_completion = 0;
        auto positions = nlohmann::json::array();
        bool complete = true;
        for (const auto peer : call.peer_observations) {
            const auto & participant = *calls.at(peer);
            if (participant.communication_nodes.size() != 1) {
                complete = false;
                break;
            }
            const auto device = participant.communication_nodes.front();
            const auto interval = raw_interval_ns(graph, device);
            const auto position = measurement_position(graph.runtime_observations().at(peer));
            if (!interval || !position) {
                complete = false;
                break;
            }

            uint64_t ready = 0;
            bool has_input = false;
            for (const auto edge_id : source.incoming_edge_ids(device)) {
                const auto & edge = graph.edge(edge_id);
                if (!edge.active) continue;
                const auto parent = raw_interval_ns(graph, edge.src);
                if (!parent || parent->second > interval->first) {
                    complete = false;
                    break;
                }
                ready = std::max(ready, parent->second);
                has_input = true;
            }
            if (!complete || !has_input) {
                complete = false;
                break;
            }
            first_ready = std::min(first_ready, ready);
            last_ready = std::max(last_ready, ready);
            last_completion = std::max(last_completion, interval->second);
            positions.push_back(*position);
        }
        if (!complete || last_completion < last_ready) continue;

        auto features = work.features;
        // Execution queries use integer-microsecond arrival coordinates.
        // Project both observed arrivals to that same grid before subtracting;
        // a sub-tick skew must not make a same-tick prediction out of domain.
        // Keep the measured completion interval below at native precision.
        features["arrival_span_us"] = static_cast<double>(last_ready / 1'000 - first_ready / 1'000);
        rows.push_back({
            {       "position",                "collective_completion:" + positions.dump() },
            {      "operation",                                             work.operation },
            { "implementation",                                        work.implementation },
            {       "features",                                        std::move(features) },
            {    "duration_us", static_cast<double>(last_completion - last_ready) / 1000.0 },
            {   "timing_scope",                           "collective_completion_interval" }
        });
    }
    return rows;
}

} // namespace markov::trace_graph::source_dag
