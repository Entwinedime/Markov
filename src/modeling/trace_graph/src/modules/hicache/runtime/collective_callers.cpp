#include "markov/trace_graph/adapters/frameworks/sglang/hicache_observations.hpp"
#include "markov/trace_graph/adapters/frameworks/sglang/source_calls.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/cost/host_operation.hpp"
#include "markov/trace_graph/modules/hicache/execution_boundaries.hpp"
#include "markov/trace_graph/modules/hicache/runtime/collective_services.hpp"
#include "markov/trace_graph/modules/hicache/runtime/host_removal.hpp"
#include "modules/hicache/runtime/source_preparation.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace markov::trace_graph::modules::hicache::runtime {

void HiCacheCollectiveServices::bind_callers(core::DagGraph & graph, const HiCacheSourcePreparation & prepared,
                                             std::span<const adapters::sglang::DeviceCollectiveCall> calls, uint64_t begin_us, uint64_t end_us) {
    const auto & original = prepared.index.graph();
    using Ref = core::DagNodeRef;
    using Kind = core::DagEdgeKind;
    struct Selected {
        const adapters::sglang::DeviceCollectiveCall * call;
        std::vector<adapters::sglang::CollectiveCallerInterval> intervals;
    };
    std::vector<Selected> selected;
    std::vector<core::TraceEvent> points;
    const auto & original_index = prepared.index;
    const auto & queues = prepared.queues;
    for (const auto & call : calls) {
        const auto & event = original.runtime_observations().at(call.region.observation);
        const auto end = core::checked_add_u64(event.ts, event.dur, "Collective caller end overflow");
        if (!is_admitted_execution_call(event, begin_us, end_us)) continue;
        const auto tasks = source_dag::observe_queued_call_tasks(original_index, queues, call.region);
        const auto intervals = tasks ? adapters::sglang::observe_collective_caller_intervals(original_index, queues, call, *tasks) : std::nullopt;
        if (!intervals) throw std::runtime_error("Collective caller needs a supported event protocol with proven stream handoffs");
        selected.push_back({ &call, *intervals });
        for (const auto at : { event.ts, end }) {
            core::TraceEvent point;
            point.name = "collective.caller";
            point.pid = event.pid;
            point.tid = event.tid;
            point.ts = at;
            points.push_back(std::move(point));
        }
    }
    if (selected.empty()) return;

    const auto boundaries = bind_hicache_control_points(graph, points);
    const adapters::sglang::HiCacheSourceIndex source(graph);
    std::vector<HiCacheHostTemplate> hosts;
    std::vector<HiCacheHostRegion> regions;
    std::vector<core::DagSyntheticNodeMutation> nodes;
    std::vector<core::DagAddEdgeMutation> edges;
    std::vector<core::DagRedirectEdgeMutation> handoffs;
    std::map<std::string, CallerSegment> segments;
    hosts.reserve(selected.size());
    for (size_t i = 0; i < selected.size(); ++i) {
        const auto & selected_call = selected[i];
        const auto & call = *selected_call.call;
        const auto steps = structure::hicache::ordinary_collective_caller_steps(call.transfer);
        const auto & event = original.runtime_observations().at(call.region.observation);
        if (!boundaries[2 * i] || !boundaries[2 * i + 1]) throw std::runtime_error("Collective caller replacement lacks exact source entry/exit boundaries");
        const auto entry = *boundaries[2 * i], exit = *boundaries[2 * i + 1];
        hosts.push_back({ .main = source.call_interval_ownership(event.pid, event.tid, event.ts, event.dur) });
        regions.push_back({ entry, exit, &hosts.back() });
        const auto lane = std::string(graph.node_lane_key(entry));
        auto previous = Ref::existing(entry);
        for (const auto & interval : selected_call.intervals) {
            const auto & step = steps[interval.step];
            const auto identity = "collective:" + std::to_string(call.region.observation) + ":caller:" + std::string(step.name);
            const auto service = Ref::synthetic(identity);
            nodes.push_back({
                identity,
                { .name = "target collective " + std::string(step.name),
                  .category = "collective_caller",
                  .lane_key = lane,
                  .attrs = { { "collective_source_observation", std::to_string(call.region.observation) },
                             { "collective_caller_step", std::string(step.name) } },
                  .observed_point = core::DagObservedPoint{ event.pid, event.tid, interval.begin_us, graph.node(entry).gpu_id } }
            });
            segments.emplace(identity, CallerSegment{ call.region.observation, interval.step, call.transfer });
            edges.push_back({ previous, service, Kind::Sequential });
            previous = service;
            if (!interval.submission) continue;

            // Completion of this CPU segment releases precisely this task.
            // Subsequent caller preparation runs concurrently with its worker.
            const auto queued = Ref::synthetic(identity + ":queued");
            nodes.push_back({
                queued.synthetic_id,
                { .name = "target collective queued",
                               .category = "execution_gate",
                               .lane_key = lane,
                               .observed_point = core::DagObservedPoint{ event.pid, event.tid, interval.end_us, graph.node(entry).gpu_id } }
            });
            edges.push_back({ service, queued, Kind::Sequential });
            previous = queued;
            for (const auto id : source.outgoing_edge_ids(*interval.submission)) {
                const auto & edge = graph.edge(id);
                if (edge.kind != Kind::Correlation) continue;
                handoffs.push_back({ .edge_index = id, .src = queued });
            }
        }
        edges.push_back({ previous, Ref::existing(exit), Kind::Sequential });
    }

    auto plan = plan_host_removal(source, regions, {}, handoffs);
    plan.component = "phase_collective_caller";
    plan.synthetic_nodes = std::move(nodes);
    plan.add_edges.insert(plan.add_edges.end(), edges.begin(), edges.end());
    const auto mutation = core::apply_dag_mutation_plan(graph, plan);
    owned_.insert(plan.disable_nodes.begin(), plan.disable_nodes.end());
    for (const auto & item : selected) owned_.insert(item.call->region.caller.owned_node_ids.begin(), item.call->region.caller.owned_node_ids.end());
    for (const auto & [identity, segment] : segments) caller_segments_.emplace(mutation.synthetic_node_ids.at(identity), segment);
}

std::optional<uint64_t> HiCacheCollectiveServices::caller_cost(core::DagGraph & graph, size_t node, cost::OperatorCostProvider & provider) const {
    const auto found = caller_segments_.find(node);
    if (found == caller_segments_.end()) return std::nullopt;
    const auto & segment = found->second;
    const auto steps = structure::hicache::ordinary_collective_caller_steps(segment.transfer);
    const auto work = structure::hicache::collective_caller_work(work_.at(segment.observation), steps[segment.step]);
    const auto estimate = cost::estimate_host_operation(provider, work, false);

    auto & event = graph.mutable_event_for_node(node);
    event.set_arg("cost_binding_source", "collective_caller_independent_model");
    event.set_arg("cost_operation", work.operation);
    event.set_arg("cost_implementation", work.implementation);
    event.set_arg("cost_timing_scope", work.timing_scope);
    event.set_arg("cpu_service_us", std::to_string(estimate.main_us));
    event.set_arg("residual_interval_us", std::to_string(estimate.main_residual_us));
    return core::checked_add_u64(estimate.main_us, estimate.main_residual_us, "Collective caller interval overflow");
}

} // namespace markov::trace_graph::modules::hicache::runtime
