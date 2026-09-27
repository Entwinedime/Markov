#include "markov/trace_graph/modules/hicache/patch/applied_validator.hpp"
#include "markov/trace_graph/modules/hicache/patch/layer_wait_insertion.hpp"
#include "markov/trace_graph/modules/hicache/patch/layer_wait_patch.hpp"
#include "markov/trace_graph/simulation/topological_simulator.hpp"
#include <algorithm>
#include <stdexcept>

using namespace markov::trace_graph;
using namespace markov::trace_graph::modules::hicache;
namespace {
void require(bool value, const char * message) {
    if (!value) throw std::runtime_error(message);
}
core::TraceEvent event(const char * name, uint64_t start, uint64_t duration, const char * lane = "main", const char * correlation = "") {
    core::TraceEvent e;
    e.name = name;
    e.ts = start;
    e.dur = duration;
    e.pid = "1";
    e.tid = lane;
    e.cat = "cpu_op";
    e.source_channel = core::TraceSourceChannel::Torch;
    e.set_arg("correlation_id", correlation);
    return e;
}
core::DagGraph fixture(bool integrated = false) {
    std::vector<core::TraceEvent> events{ event("before", 0, 5),
                                          event("enqueue wait", 10, 2, "main", "0"),
                                          event("enqueue previous", 20, 2, "main", "1"),
                                          event("enqueue next", 40, 2, "main", "2"),
                                          event("AscendCL@aclrtSynchronizeStream", 50, 2),
                                          event("AscendCL@aclrtStreamWaitEvent", 11, 1, "worker", "0"),
                                          event("launch previous", 25, 2, "worker", "1"),
                                          event("launch next", 45, 2, "worker", "2"),
                                          event("record", 0, 100, "H2D"),
                                          event("wait", 110, 0, "compute"),
                                          event("previous kernel", 200, 4, "compute"),
                                          event("next kernel", 210, 5, "compute"),
                                          event("new layer ready", 0, 120, "H2D") };
    if (integrated) {
        events.push_back(event("source transfer", 0, 4, "H2D"));
        events.back().set_arg("size(B)", "400");
    }
    for (size_t i = 1; i <= 3; ++i) events[i].cat = "enqueue";
    core::DagGraph graph(std::move(events), 0);
    for (size_t i = 0; i < graph.events().size(); ++i) graph.add_node(i, i < 8, graph.events()[i].tid);
    for (size_t i = 1; i < 5; ++i) {
        graph.add_edge(i - 1, i, core::DagEdgeKind::Sequential);
        const auto & before = graph.event_for_node(i - 1);
        graph.mutable_node(i - 1).cpu_gap_after = graph.mutable_node(i - 1).original_cpu_gap_after = graph.event_for_node(i).ts - before.ts - before.dur;
    }
    for (size_t i = 1; i <= 3; ++i) {
        graph.add_edge(i, i + 4, core::DagEdgeKind::Correlation);
        graph.add_edge(i + 4, i + 8, core::DagEdgeKind::Correlation);
        if (i > 1) graph.add_edge(i + 3, i + 4, core::DagEdgeKind::Sequential);
    }
    graph.add_edge(8, 9, core::DagEdgeKind::Sync);
    graph.add_edge(9, 10, core::DagEdgeKind::Stream);
    graph.add_edge(10, 11, core::DagEdgeKind::Stream);
    graph.add_edge(10, 4, core::DagEdgeKind::Sync);
    if (integrated) graph.add_edge(13, 8, core::DagEdgeKind::Stream);
    auto batch = event("runtime.hicache.layer_waits", 7, 10);
    batch.set_arg("wait_clock", "profiler_ns");
    batch.set_arg("status", "returned");
    batch.set_arg("request_ids", "[\"donor\"]");
    batch.set_arg("phase", "EXTEND");
    batch.set_arg("consumer_index", "0");
    batch.set_arg("layer_count", "2");
    batch.set_arg("wait_intervals", "[[0,8000,16000]]");
    auto inactive = batch;
    inactive.ts = 30;
    inactive.dur = 2;
    inactive.set_arg("request_ids", "[\"request\"]");
    inactive.set_arg("consumer_index", "-1");
    inactive.set_arg("wait_intervals", "[[0,30100,30600],[1,31100,31600]]");
    if (integrated) inactive.set_arg("wait_intervals", "[[0,30100,30600],[0,31100,31600]]");
    graph.set_runtime_observations({ batch, inactive });
    if (integrated) {
        auto fact = event("lookup", 0, 0);
        fact.source_channel = core::TraceSourceChannel::PythonProbe;
        fact.set_arg("fact", R"({"class":"workload_identity","role":"cache_lookup_input","consumers":["hicache_dag_patch"]})");
        graph.set_hicache_fact_events({ fact });
    }
    return graph;
}

void check_integrated_insertion(bool remove_donor, bool rebind = false) {
    auto graph = fixture(true);
    const patch::HiCacheSourceDagIndex source(graph);
    const auto waits = observe_hicache_layer_waits(source);
    require(waits.status == "ready", "integration uses fully observed calls");
    patch::HiCacheRewriteDecision donor;
    donor.request_id = "donor";
    donor.source_fact_node_id = source.fact_nodes().front().node_id;
    donor.target_effect_state = remove_donor ? model::HiCacheTargetEffectState::NotRequired : model::HiCacheTargetEffectState::Required;
    donor.source_readiness_topology_reused = true;
    donor.owned_duration_nodes = { 13 };
    patch::HiCacheRewriteDecision target;
    target.effect_id = target.synthetic_id = "new_load";
    target.request_id = "request";
    target.source_fact_node_id = donor.source_fact_node_id;
    target.source_execution_anchor_node_id = 2;
    target.target_effect_state = model::HiCacheTargetEffectState::Required;
    target.rewrite_kind = patch::HiCacheRewriteKind::InsertIo;
    target.shadow_plan_ready = true;
    target.duration_us = 101;
    target.resource_lane = "H2D";
    target.consumer_dependency_required = true;
    target.consumer_anchors = { 3 };
    if (rebind) {
        target.request_id = "donor";
        target.eligibility_timestamp_us = 1;
        target.source_execution_anchor_node_id = 0;
    }
    core::DagMutationPlan plan{ .component = "hicache" };
    plan.synthetic_nodes.push_back({
        .synthetic_id = "new_load",
        .node = { .name = "new load", .is_cpu = false, .lane_key = "H2D", .duration = 101 },
        .effect_id = "new_load"
    });
    plan.add_edges.push_back({ .src = core::DagNodeRef::existing(*target.source_execution_anchor_node_id),
                               .dst = core::DagNodeRef::synthetic("new_load"),
                               .kind = core::DagEdgeKind::Mutation,
                               .effect_id = "new_load" });
    plan.add_edges.push_back(
        { .src = core::DagNodeRef::synthetic("new_load"), .dst = core::DagNodeRef::existing(3), .kind = core::DagEdgeKind::Mutation, .effect_id = "new_load" });
    std::vector<patch::HiCacheRewriteDecision> decisions{ donor, target };
    if (rebind) {
        auto inactive = donor;
        inactive.request_id = "request";
        decisions.push_back(inactive);
    }
    // Force a late conflict after candidate expansion; neither decisions nor
    // the caller's plan may retain any part of a rejected transaction.
    if (!rebind) {
        auto conflict = plan;
        conflict.set_cpu_gaps.push_back({ .node_id = 2, .duration = 18 });
        const auto rejected = patch::append_hicache_layer_wait_plan(source, waits, decisions, conflict);
        require(rejected.status == "blocked" && conflict.synthetic_nodes.size() == 1 && conflict.add_edges.size() == 2 && decisions[1].layer_io.empty(),
                "a late insertion conflict rolls back layout and mutation together");
    }
    const auto audit = patch::append_hicache_layer_wait_plan(source, waits, decisions, plan);
    require(audit.status == "ready" && audit.inserted_calls == (rebind ? 0 : 2) && audit.expanded_loadbacks == 1
                && audit.rebound_calls == static_cast<size_t>(rebind) && audit.removed_calls == static_cast<size_t>(remove_donor && !rebind),
            "production transaction supports insertion, rebinding and mixed removal");
    require(decisions[1].layer_io.size() == 1 && decisions[1].layer_io.front().waits.size() == (rebind ? 1 : 2),
            "waits bind to the predicted Record, not the donor Record");
    const auto mutation = core::apply_dag_mutation_plan(graph, plan);
    patch::HiCacheShadowRewriteTransaction shadow;
    shadow.decisions = { decisions[1] };
    const auto proof = patch::validate_hicache_applied_patch(graph, shadow, {}, plan, mutation, true);
    require(proof.plan_journal_exact && proof.records.front().ready, "integrated insertion passes applied validation");
    const auto replay = simulation::run_topological_simulation(graph);
    require(replay.cpu_task_count == (rebind ? 3 : remove_donor ? 4 : 5), "inserted and removed workers share the observed FIFO");
    const auto ready = mutation.synthetic_node_ids.at(decisions[1].layer_io.front().ready_id);
    require(graph.node(11).completion_time == graph.node(ready).completion_time + (rebind ? 9 : 5), "new layer readiness controls the existing continuation");
}
} // namespace

void check_hicache_layer_wait_insertion() {
    check_integrated_insertion(false);
    check_integrated_insertion(true);
    check_integrated_insertion(true, true);
    const auto graph = fixture();
    const patch::HiCacheSourceDagIndex source(graph);
    const auto waits = observe_hicache_layer_waits(source);
    require(waits.status == "ready" && waits.calls.size() == 3, "active and inactive source call ownership is complete");
    const std::vector<patch::HiCacheLayerWaitInsertion> calls{
        { &waits.calls[2], core::DagNodeRef::existing(12) },
        { &waits.calls[1], core::DagNodeRef::existing(12) }
    };
    const auto insertion = patch::plan_hicache_layer_wait_insertions(source, waits, calls);
    require(insertion.status == "ready" && insertion.inserted_calls == 2, "two inactive calls sharing a gap can be inserted together");
    require(insertion.mutation.set_cpu_gaps.size() == 1 && insertion.mutation.set_cpu_gaps.front().duration == 8,
            "shared gap is split once and retains the interval before the first call");
    uint64_t outside_gap = 8, main_cost = 0, worker_cost = 0;
    for (const auto & node : insertion.mutation.synthetic_nodes) {
        outside_gap += node.node.cpu_gap_after;
        if (node.node.cpu_task_ready_delay_us) worker_cost += node.node.duration;
        else if (node.node.is_cpu) main_cost += node.node.duration;
    }
    require(outside_gap == 17 && main_cost == 16 && worker_cost == 2, "two 500 ns returns are removed in aggregate; outside gap stays separate from service");
    for (const auto ready : { 5, 120, 150 }) {
        auto target = graph;
        target.set_node_duration(12, ready);
        const auto applied = core::apply_dag_mutation_plan(target, insertion.mutation);
        const auto full = simulation::run_topological_simulation(target);
        require(full.cpu_queue_count == 1 && full.cpu_task_count == 5, "new waits enter the observed worker FIFO");
        require(target.node(3).completion_time == 57, "main program acquires measured call costs, not device waiting time");
        const auto expected = static_cast<uint64_t>(std::max(104, ready));
        require(target.node(11).completion_time == expected + 5 && target.node(4).completion_time == expected + 2,
                "device continuation and later CPU synchronization both wait for the inserted readiness");
        require(simulation::run_control_topological_simulation(target).e2e_us == full.e2e_us, "queue materialization retains inserted dependencies on replay");
        for (const auto & [name, id] : applied.synthetic_node_ids)
            require(target.event_for_node(id).ts == 0, "modeled calls must not fabricate observed timestamps");
    }
    auto busy = graph;
    busy.set_node_duration(6, 200);
    (void)core::apply_dag_mutation_plan(busy, insertion.mutation);
    (void)simulation::run_topological_simulation(busy);
    require(busy.node(11).completion_time == 237, "new workers queue behind busy work instead of running independently");
    auto recorded = graph;
    recorded.mutable_event_for_node(4).name = "AscendCL@aclrtSynchronizeEvent";
    const patch::HiCacheSourceDagIndex recorded_source(recorded);
    const auto fixed_event = patch::plan_hicache_layer_wait_insertions(recorded_source, waits, calls);
    require(fixed_event.status == "ready", "fixed-event synchronization keeps its original dependency");
    (void)core::apply_dag_mutation_plan(recorded, fixed_event.mutation);
    (void)simulation::run_topological_simulation(recorded);
    require(recorded.node(4).completion_time == 106 && recorded.node(11).completion_time == 125,
            "an earlier recorded event must not acquire the later inserted layer wait");
    auto missing = waits;
    missing.calls[0].phase = "DECODE";
    const auto rejected = patch::plan_hicache_layer_wait_insertions(source, missing, calls);
    require(rejected.status == "blocked" && rejected.mutation.empty(), "missing same-phase cost samples cannot silently become zero cost");
    auto unknown = graph;
    for (size_t id = 0; id < unknown.edge_count(); ++id)
        if (unknown.edge(id).src == 2 && unknown.edge(id).dst == 6) unknown.disable_edge(id);
    const patch::HiCacheSourceDagIndex unknown_source(unknown);
    const auto boundary = patch::plan_hicache_layer_wait_insertions(unknown_source, waits, calls);
    require(boundary.status == "blocked" && boundary.mutation.empty(), "device timestamps cannot substitute for an unknown submission boundary");
}
