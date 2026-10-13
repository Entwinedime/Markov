#include "markov/trace_graph/modules/hicache/runtime/write_confirmations.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/modules/hicache/execution_boundaries.hpp"
#include "markov/trace_graph/modules/hicache/runtime/host_removal.hpp"
#include "markov/trace_graph/modules/hicache/runtime/load_execution.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_calls.hpp"
#include "markov/trace_graph/structure/hicache/confirmation.hpp"
#include "markov/trace_graph/structure/missing_work.hpp"
#include <algorithm>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <utility>

namespace markov::trace_graph::modules::hicache::runtime {
namespace {
core::TraceEvent boundary(const std::string & pid, const std::string & tid, uint64_t at) {
    core::TraceEvent point;
    point.name = "write_confirmation_boundary";
    point.pid = pid;
    point.tid = tid;
    point.ts = at;
    return point;
}

} // namespace

ConfirmationExpansion generate_write_confirmation(const core::DagGraph & graph, size_t anchor, std::optional<size_t> worker,
                                                  std::span<const structure::hicache::WriteConfirmationStep> steps) {
    ConfirmationExpansion generated;
    auto & plan = generated.plan;
    const auto lane = std::string(graph.node_lane_key(anchor));
    std::optional<size_t> previous;
    for (const auto & step : steps) {
        std::optional<size_t> call_previous;
        const auto main = plan.append_main(lane, "target " + step.work.operation, call_previous);
        plan.nodes[main].work.attrs = {
            {           "component",                       step.work.component },
            {           "operation",                       step.work.operation },
            {      "implementation",                  step.work.implementation },
            {      "cost_operation",                       step.work.operation },
            { "cost_implementation",                  step.work.implementation },
            {   "cost_timing_scope",                             "cpu_service" },
            {       "work_features", nlohmann::json(step.work.features).dump() }
        };
        const auto returned = plan.append_main(lane, "after " + step.work.operation, call_previous, "");
        if (previous) plan.edges.push_back({ *previous, main, core::DagEdgeKind::Sequential });
        if (step.completed_batch) {
            plan.event_waits.push_back({ core::DagNode::kNoNode, main });
            generated.acknowledgements.push_back(returned);
        }
        HostOperationSlots slots{ .main = main };
        cost::OperationCostRequest request{ .work = step.work };
        if (step.destroys_event) {
            if (!worker)
                throw structure::MissingWork("Write confirmation requires the scheduler's CPU worker queue",
                                             {
                                                 { "write_confirmation_worker", 1 }
                });
            slots.worker = plan.nodes.size();
            const auto task = structure::hicache::event_destruction_work();
            request.worker_work = task;
            plan.nodes.push_back({
                { .name = "target event destruction",
                 .category = "hicache_patch",
                 .lane_key = std::string(graph.node_lane_key(*worker)),
                 .attrs = { { "component", task.component },
                             { "operation", task.operation },
                             { "implementation", task.implementation },
                             { "cost_operation", task.operation },
                             { "cost_implementation", task.implementation },
                             { "cost_timing_scope", task.timing_scope } } },
                returned,
                *worker
            });
        }
        generated.operations.push_back({ std::move(request), { std::move(slots) } });
        previous = returned;
        plan.host_return = returned;
    }
    return generated;
}

void HiCacheWriteConfirmations::prepare(core::DagGraph & graph, uint64_t begin, uint64_t end, const std::vector<core::TraceEvent> & replaced,
                                        std::optional<uint64_t> idle_since_us) {
    graph_ = &graph;
    const auto & observations = graph.runtime_observations();
    for (const auto & event : observations) {
        if (event.name != "runtime.hicache.write_completion" || event.arg("status") != "returned" || event.arg("blocking") != "false" || event.ts > end
            || (event.ts < begin && (!idle_since_us || event.ts < *idle_since_us)))
            continue;
        if (std::ranges::any_of(replaced, [&](const auto & span) {
                return event.pid == span.pid && event.tid == span.tid && span.ts <= event.ts && span.ts + span.dur >= event.ts + event.dur;
            }))
            continue;

        const core::TraceEvent * reduction = nullptr;
        for (const auto & item : observations)
            if (item.name == "runtime.cpu_collective" && item.arg("role") == "write_completion_check" && item.pid == event.pid && item.tid == event.tid
                && item.ts >= event.ts && item.ts + item.dur <= event.ts + event.dur) {
                if (reduction) throw std::runtime_error("Write confirmation has multiple MIN calls");
                reduction = &item;
            }
        if (!reduction) throw std::runtime_error("Write confirmation has no MIN return");
        // This outer check began by the HTTP endpoint. Keep its complete MIN
        // and acknowledgement tail, including a MIN beginning in causal-tail
        // context. Background work does not extend the scoring endpoint.
        if (reduction->ts < begin) continue;
        tails_.push_back({ event.pid, event.tid, reduction->ts + reduction->dur, event.ts + event.dur });
    }
}

void HiCacheWriteConfirmations::replace_source_tails(core::DagGraph & graph) {
    std::vector<core::TraceEvent> points;
    for (const auto & tail : tails_) {
        points.push_back(boundary(tail.pid, tail.tid, tail.begin));
        points.push_back(boundary(tail.pid, tail.tid, tail.end));
    }
    const auto bound = bind_hicache_control_points(graph, points);
    const patch::HiCacheSourceDagIndex source(graph);
    const auto queues = simulation::detail::discover_cpu_task_queues(graph);
    std::vector<HiCacheHostTemplate> hosts;
    hosts.reserve(tails_.size());
    for (const auto & tail : tails_) hosts.push_back(observe_host_template(source, tail.pid, tail.tid, tail.begin, tail.end));

    std::vector<HiCacheHostRegion> regions;
    for (size_t i = 0; i < hosts.size(); ++i) {
        if (!bound[2 * i] || !bound[2 * i + 1]) throw std::runtime_error("Confirmation replacement lost its boundaries");
        regions.push_back({ *bound[2 * i], *bound[2 * i + 1], &hosts[i] });
        const auto key = Lane{ tails_[i].pid, tails_[i].tid };
        const auto compute = compute_resources_.find(key);
        if (!worker_members_.contains(key) && compute != compute_resources_.end()) {
            if (const auto resource = observe_load_index_resources(source, queues, *bound[2 * i], compute->second))
                worker_members_.emplace(key, resource->worker);
        }
    }
    (void)core::apply_dag_mutation_plan(graph, plan_host_removal(source, regions));
}

void HiCacheWriteConfirmations::rebind_resources(const std::map<size_t, size_t> & workers) {
    for (auto & [lane, member] : worker_members_) member = workers.at(graph_->node(member).lane_id);
}

size_t HiCacheWriteConfirmations::expand(const WriteConfirmationWork & work, model::HiCacheState & state, const HiCacheWriteCalls & writes,
                                         simulation::FutureDag & future) {
    const auto pending = state.unacknowledged_device_writes(work.fact);
    if (pending.size() < work.confirmed_writes) throw std::logic_error("Sampled confirmation prefix is no longer pending");
    std::vector<size_t> events;
    std::vector<structure::hicache::WriteConfirmationBatch> batches;
    for (size_t i = 0; i < work.confirmed_writes; ++i) {
        events.push_back(writes.completion_for(pending[i]));
        batches.push_back(
            { core::checked_multiply_u64(pending[i].header.pages.size(), page_size_, "Confirmation token count overflow"), pending[i].published_nodes });
    }

    const auto anchor = *work.fact.execution_anchor_node_id;
    const auto member = worker_members_.find({ work.fact.pid, work.fact.tid });
    const auto worker = member == worker_members_.end() ? std::nullopt : std::optional{ member->second };
    const auto program = structure::hicache::write_confirmation_program(batches);
    auto generated = generate_write_confirmation(*graph_, anchor, worker, program);
    bind_host_operations(generated.plan, generated.operations, operator_provider_, "confirmation_independent_model");
    const auto expanded = expand_host(generated.plan, {}, future, anchor, events);
    for (const auto returned : generated.acknowledgements) acknowledgements_.emplace(expanded.nodes.at(returned), work.fact);
    return expanded.host_return;
}

void HiCacheWriteConfirmations::advance(size_t node, uint64_t time_us, model::HiCacheState & state) {
    const auto found = acknowledgements_.find(node);
    if (found == acknowledgements_.end()) return;

    auto fact = found->second;
    fact.ts = time_us;
    fact.execution_anchor_node_id = node;
    // Host publication and storage enqueue happen in the acknowledgement,
    // before event destruction and before the whole confirmation returns.
    state.acknowledge_writes(fact, 1);
}

} // namespace markov::trace_graph::modules::hicache::runtime
