#include "markov/trace_graph/modules/hicache/runtime/collective_services.hpp"
#include "markov/trace_graph/adapters/frameworks/sglang/hicache_observations.hpp"
#include "markov/trace_graph/adapters/frameworks/sglang/source_calls.hpp"
#include "markov/trace_graph/core/dag_builder.hpp"
#include "markov/trace_graph/core/dag_mutation.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/modules/hicache/execution_boundaries.hpp"
#include "markov/trace_graph/structure/ascend_event.hpp"
#include "markov/trace_graph/structure/missing_work.hpp"
#include "modules/hicache/runtime/source_preparation.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <utility>

namespace markov::trace_graph::modules::hicache::runtime {

void HiCacheCollectiveServices::bind(core::DagGraph & graph, const HiCacheSourcePreparation & prepared,
                                     std::span<const adapters::sglang::DeviceCollectiveCall> calls, uint64_t begin_us, uint64_t end_us) {
    const auto & original = prepared.index.graph();
    using Ref = core::DagNodeRef;
    using Kind = core::DagEdgeKind;
    core::DagMutationPlan plan{ .component = "phase_device_collective" };
    struct Replacement {
        size_t observation;
        Ref arrival, service;
        std::optional<structure::hicache::CollectiveTaskKind> event_kind;
    };
    std::map<size_t, Replacement> replacements;
    std::map<size_t, Ref> arrivals;
    struct WorkerReplacement {
        size_t observation, submission;
        Ref launch, returned;
        structure::hicache::CollectiveTaskKind kind;
        std::optional<size_t> device;
    };
    std::map<size_t, WorkerReplacement> worker_replacements;
    const auto & source = prepared.index;
    const auto & queues = prepared.queues;
    for (const auto & call : calls) {
        const auto & event = original.runtime_observations().at(call.region.observation);
        if (!is_admitted_execution_call(event, begin_us, end_us)) continue;
        if (call.communication_nodes.size() != 1)
            throw std::runtime_error("Target collective needs a lowering for its observed multi-node communication protocol");
        const auto node = call.communication_nodes.front();
        if (!graph.node(node).active) throw std::runtime_error("Communication service was already removed by another structural owner");

        const auto identity = "collective:" + std::to_string(call.region.observation);
        const auto arrival = Ref::synthetic(identity + ":input_ready");
        const auto service = Ref::synthetic(identity + ":service");
        const auto lane = std::string(graph.node_lane_key(node));
        const auto & device_event = graph.event_for_node(node);
        const core::DagObservedPoint location{ device_event.pid, device_event.tid, device_event.ts, graph.node(node).gpu_id };
        plan.synthetic_nodes.push_back({
            arrival.synthetic_id,
            { .name = "target collective input ready", .category = "execution_gate", .is_cpu = false, .lane_key = lane, .observed_point = location }
        });
        plan.synthetic_nodes.push_back({
            service.synthetic_id,
            { .name = "target collective service",
                           .category = "collective_service",
                           .is_cpu = false,
                           .lane_key = lane,
                           .attrs = { { "collective_source_observation", std::to_string(call.region.observation) },
                         { "collective_source_node", std::to_string(node) },
                         { "collective_layer", std::to_string(call.layer_index) },
                         { "collective_role", call.role },
                         { "collective_callsite", call.callsite },
                         { "collective_members", nlohmann::json(call.members).dump() },
                         { "collective_global_rank", std::to_string(call.global_rank) } },
                           .observed_point = location }
        });
        if (!replacements.emplace(node, Replacement{ call.region.observation, arrival, service }).second)
            throw std::runtime_error("Communication device node has multiple call owners");
        arrivals.emplace(call.region.observation, arrival);
        peers_.emplace(call.region.observation, call.peer_observations);
        plan.disable_nodes.push_back(node);

        const auto & tasks = call.queued_tasks;
        if (!tasks) throw std::runtime_error("Collective CPU replacement lacks complete queued-task attribution");
        const auto task = std::ranges::find_if(*tasks, [&](const auto & candidate) { return candidate.devices.contains(node); });
        if (task == tasks->end() || task->devices.size() != 1)
            throw std::runtime_error("Collective CPU replacement needs one logical communication launch per reduction task");
        if (adapters::sglang::collective_task_kind(original.event_for_node(queues.tasks.at(task->task).submission).name) != call.transfer)
            throw std::runtime_error("Collective CPU submission does not match its declared HCCL transfer");

        for (const auto & observed : *tasks) {
            using TaskKind = structure::hicache::CollectiveTaskKind;
            const auto & queued = queues.tasks.at(observed.task);
            const auto primitive = adapters::sglang::collective_task_kind(original.event_for_node(queued.submission).name);
            if (!primitive) throw std::runtime_error("Collective call contains an unsupported queued CPU primitive");
            const bool cpu_only = *primitive == TaskKind::Destroy;
            if (observed.devices.size() != (cpu_only ? 0 : 1)) throw std::runtime_error("Collective queue primitive lacks its expected device-event ownership");
            const auto device = cpu_only ? std::nullopt : std::optional{ *observed.devices.begin() };
            if (device) {
                const auto & name = original.event_for_node(*device).name;
                // The builder reconstructs waits omitted by the device trace
                // from the CPU submission, recorded event and stream position.
                // That dependency is a valid wait, not a measured service.
                const bool event_wait = name == "EVENT_WAIT" || (name == "logical_event_wait" && original.node(*device).kind == core::DagNodeKind::Synthetic);
                if ((*primitive == TaskKind::Record && name != "EVENT_RECORD") || (*primitive == TaskKind::Wait && !event_wait)
                    || ((*primitive == TaskKind::Reduce || *primitive == TaskKind::Gather) && *device != node))
                    throw std::runtime_error("Collective queued task contradicts its device operation: pid=" + event.pid + " at_us=" + std::to_string(event.ts)
                                             + " submission=" + original.event_for_node(queued.submission).name + " device=" + name
                                             + " node=" + std::to_string(*device) + " communication=" + std::to_string(node));
            }

            const auto task_identity = identity + ":task:" + std::to_string(observed.task);
            const auto launch = Ref::synthetic(task_identity + ":worker_launch");
            const auto returned = cpu_only ? launch : Ref::synthetic(task_identity + ":worker_return");
            const auto primitive_name = std::string(structure::hicache::collective_task_name(*primitive));
            if (device && (*primitive == TaskKind::Record || *primitive == TaskKind::Wait)) {
                const auto event_service = Ref::synthetic(task_identity + ":device_event");
                const auto & original_event = original.event_for_node(*device);
                const core::DagObservedPoint event_location{ original_event.pid, original_event.tid, original_event.ts, graph.node(*device).gpu_id };
                plan.synthetic_nodes.push_back({
                    event_service.synthetic_id,
                    { .name = original_event.name,
                                   .category = "collective_event_service",
                                   .is_cpu = false,
                                   .lane_key = std::string(graph.node_lane_key(*device)),
                                   .attrs = { { "collective_source_observation", std::to_string(call.region.observation) },
                                 { "collective_event_source_node", std::to_string(*device) },
                                 { "collective_queue_primitive", primitive_name } },
                                   .observed_point = event_location }
                });
                // Incoming stream/record dependencies express waiting. Only
                // the event's own measured service is assigned to this node.
                if (!replacements.emplace(*device, Replacement{ call.region.observation, event_service, event_service, *primitive }).second)
                    throw std::runtime_error("Collective device event has multiple semantic owners");
                plan.disable_nodes.push_back(*device);
            }

            const auto worker_lane = std::string(graph.node_lane_key(queued.first));
            const auto & worker_event = original.event_for_node(queued.first);
            const core::DagObservedPoint worker_location{ worker_event.pid, worker_event.tid, worker_event.ts, graph.node(queued.first).gpu_id };
            const auto slots = cpu_only ? std::vector{ launch } : std::vector{ launch, returned };
            for (const auto & slot : slots) {
                const bool first = slot.synthetic_id == launch.synthetic_id;
                auto name = "target " + primitive_name + " worker";
                if (!cpu_only) name += first ? " launch" : " return";
                plan.synthetic_nodes.push_back({
                    slot.synthetic_id,
                    { .name = std::move(name),
                                   .category = "collective_submission",
                                   .lane_key = worker_lane,
                                   .attrs = { { "collective_source_observation", std::to_string(call.region.observation) },
                                 { "collective_queue_primitive", primitive_name },
                                 { "dispatch_cost_source", "base_scheduler_residual" } },
                                   .cpu_task_ready_delay_us = first ? original.cpu_service_ready_delay(queued.first, queued.ready_delay_us) : 0,
                                   .observed_point = worker_location }
                });
                plan.add_edges.push_back({ Ref::existing(queued.submission), slot, Kind::Correlation });
            }
            if (!cpu_only) plan.add_edges.push_back({ launch, returned, Kind::Sequential });

            size_t launch_points = 0;
            for (const auto worker : observed.workers) {
                if (!graph.node(worker).active) throw std::runtime_error("Collective worker was already replaced by another structural owner");
                const auto & member_event = original.event_for_node(worker);
                if (core::synchronizes_recorded_event(member_event) || core::synchronizes_device_frontier(member_event))
                    throw std::runtime_error("Collective worker has an unmodeled host synchronization");
                for (const auto id : source.outgoing_edge_ids(worker)) {
                    const auto & edge = original.edge(id);
                    if (edge.kind == Kind::Correlation && device && edge.dst == *device) ++launch_points;
                }
                if (!worker_replacements.emplace(worker, WorkerReplacement{ call.region.observation, queued.submission, launch, returned, *primitive, device })
                         .second)
                    throw std::runtime_error("Collective worker task has multiple semantic owners");
                plan.disable_nodes.push_back(worker);
            }
            if (launch_points != (cpu_only ? 0 : 1)) throw std::runtime_error("Collective task has no unique CPU launch boundary");
        }
    }
    if (replacements.empty()) return;

    // A service starts after every rank's original input/submission frontier.
    // This is a collective-level rendezvous approximation, not a claim that
    // HCCL's packet-level protocol cannot overlap with staggered arrivals.
    for (const auto & [node, replacement] : replacements) {
        if (replacement.event_kind) continue;
        for (const auto peer : peers_.at(replacement.observation)) {
            const auto at = arrivals.find(peer);
            if (at == arrivals.end()) throw std::runtime_error("Formal collective replacement is missing a participant");
            plan.add_edges.push_back({ at->second, replacement.service, Kind::Mutation });
        }
    }
    for (size_t edge_id = 0; edge_id < graph.edges().size(); ++edge_id) {
        const auto & edge = graph.edge(edge_id);
        if (!edge.active) continue;
        const auto from = replacements.find(edge.src), to = replacements.find(edge.dst);
        const auto worker_from = worker_replacements.find(edge.src), worker_to = worker_replacements.find(edge.dst);
        if (from == replacements.end() && to == replacements.end() && worker_from == worker_replacements.end() && worker_to == worker_replacements.end())
            continue;

        if (worker_from != worker_replacements.end() && worker_to != worker_replacements.end()
            && worker_from->second.submission == worker_to->second.submission) {
            // Source correlation chains also connect successive CPU leaves
            // of one queued task. The replacement's launch -> return chain
            // preserves that order along with the ordinary lane sequence.
            if (edge.kind != Kind::Sequential && edge.kind != Kind::Correlation)
                throw std::runtime_error("Collective worker task has an unsupported internal dependency: kind=" + core::edge_kind_name(edge.kind)
                                         + " from=" + graph.event_for_node(edge.src).name + " to=" + graph.event_for_node(edge.dst).name
                                         + " submission=" + std::to_string(worker_from->second.submission));
            plan.disable_edges.push_back(edge_id);
            continue;
        }
        if (worker_to != worker_replacements.end() && edge.kind == Kind::Correlation && edge.src == worker_to->second.submission) {
            plan.disable_edges.push_back(edge_id);
            continue;
        }

        core::DagRedirectEdgeMutation redirect{ .edge_index = edge_id };
        if (from != replacements.end()) redirect.src = from->second.service;
        if (to != replacements.end()) redirect.dst = to->second.arrival;
        if (worker_from != worker_replacements.end()) {
            if (edge.kind == Kind::Correlation && worker_from->second.device == edge.dst) { redirect.src = worker_from->second.launch; }
            else if (edge.kind == Kind::Sequential && graph.node(edge.src).lane_id == graph.node(edge.dst).lane_id) {
                redirect.src = worker_from->second.returned;
            }
            else { throw std::runtime_error("Collective worker has an unmodeled external result consumer"); }
        }
        if (worker_to != worker_replacements.end()) {
            if (edge.kind != Kind::Sequential || graph.node(edge.src).lane_id != graph.node(edge.dst).lane_id)
                throw std::runtime_error("Collective worker has an unmodeled external prerequisite");
            redirect.dst = worker_to->second.launch;
        }
        plan.redirect_edges.push_back(std::move(redirect));
    }
    const auto mutation = core::apply_dag_mutation_plan(graph, plan);
    for (const auto & [node, replacement] : replacements) {
        owned_.insert(node);
        if (replacement.event_kind) {
            event_services_.emplace(mutation.synthetic_node_ids.at(replacement.service.synthetic_id),
                                    EventService{ replacement.observation, *replacement.event_kind });
            continue;
        }
        service_observations_.emplace(mutation.synthetic_node_ids.at(replacement.service.synthetic_id), replacement.observation);
        arrival_nodes_.emplace(replacement.observation, mutation.synthetic_node_ids.at(replacement.arrival.synthetic_id));
    }
    for (const auto & [node, replacement] : worker_replacements) {
        owned_.insert(node);
        const auto part = replacement.device ? WorkerSegment::Part::Launch : WorkerSegment::Part::Whole;
        worker_segments_.emplace(mutation.synthetic_node_ids.at(replacement.launch.synthetic_id),
                                 WorkerSegment{ replacement.observation, replacement.kind, part });
        if (replacement.device)
            worker_segments_.emplace(mutation.synthetic_node_ids.at(replacement.returned.synthetic_id),
                                     WorkerSegment{ replacement.observation, replacement.kind, WorkerSegment::Part::Return });
    }
    bind_callers(graph, prepared, calls, begin_us, end_us);
}

void HiCacheCollectiveServices::set_work(const structure::hicache::DeviceCollectivePlan & plan) {
    if (!plan.service)
        throw structure::MissingWork("Target collective work unavailable",
                                     {
                                         { "source_observation", plan.source_observation },
                                         {             "reason",             plan.reason }
        });
    const auto & peers = peers_.at(plan.source_observation);
    if (plan.peer_observations != peers) throw std::runtime_error("Target collective changed its declared participant calls");
    for (const auto peer : peers) {
        const auto existing = work_.find(peer);
        if (existing != work_.end()
            && (existing->second.features != plan.service->features || existing->second.implementation != plan.service->implementation
                || existing->second.operation != plan.service->operation))
            throw std::runtime_error("Target ranks disagree on collective operation or payload");
    }
    work_.insert_or_assign(plan.source_observation, *plan.service);
}

std::optional<structure::OperationWork> HiCacheCollectiveServices::demand(const core::DagGraph & graph, size_t node) const {
    if (const auto event = event_services_.find(node); event != event_services_.end()) {
        return structure::ascend_event_work(work_.at(event->second.observation).component,
                                            event->second.kind == structure::hicache::CollectiveTaskKind::Wait ? structure::DeviceEventKind::Wait
                                                                                                               : structure::DeviceEventKind::Record);
    }

    const auto found = service_observations_.find(node);
    if (found == service_observations_.end()) return std::nullopt;

    auto work = work_.at(found->second);
    uint64_t first = std::numeric_limits<uint64_t>::max(), last = 0;
    for (const auto peer : peers_.at(found->second)) {
        const auto completed = graph.node(arrival_nodes_.at(peer)).completion_time;
        first = std::min(first, completed);
        last = std::max(last, completed);
    }
    // Every arrival is a hard predecessor and has completed at this query.
    // Skew may allow protocol work before the last input becomes ready, so
    // the remaining completion interval need not depend on bytes alone.
    work.features["arrival_span_us"] = static_cast<double>(last - first);
    // Ordinary logits all-gather has the same backend operation in both
    // phases. Share its completion model by actual work, not phase identity;
    // caller/worker models and component ownership remain phase-specific.
    if (work.implementation.starts_with("sglang.NpuCommunicator.all_gather:")) work.operation = "collective.all_gather";
    return work;
}

std::optional<uint64_t> HiCacheCollectiveServices::worker_cost(core::DagGraph & graph, size_t node, cost::OperatorCostProvider & provider) const {
    const auto found = worker_segments_.find(node);
    if (found == worker_segments_.end()) return std::nullopt;

    const auto & segment = found->second;
    auto work = structure::hicache::collective_task_work(work_.at(segment.observation), segment.kind);
    std::string scope;
    switch (segment.part) {
    case WorkerSegment::Part::Launch:
        scope = "worker_launch";
        break;
    case WorkerSegment::Part::Return:
        scope = "worker_return";
        break;
    case WorkerSegment::Part::Whole:
        scope = "worker";
        break;
    }
    work.timing_scope = scope + "_service";
    auto residual = work;
    residual.timing_scope = scope + "_residual_interval";
    const std::array requests{ work, residual };
    const auto estimates = provider.estimate_all(requests);
    auto & event = graph.mutable_event_for_node(node);
    event.set_arg("cost_binding_source", "collective_worker_independent_model");
    event.set_arg("worker_service_us", std::to_string(estimates[0].duration_us));
    event.set_arg("worker_residual_us", std::to_string(estimates[1].duration_us));
    return core::checked_add_u64(estimates[0].duration_us, estimates[1].duration_us, "Collective worker interval overflow");
}

} // namespace markov::trace_graph::modules::hicache::runtime
