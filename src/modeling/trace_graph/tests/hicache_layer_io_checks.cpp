#include "markov/trace_graph/modules/hicache/patch/applied_validator.hpp"
#include "markov/trace_graph/modules/hicache/patch/layer_io.hpp"
#include "markov/trace_graph/simulation/topological_simulator.hpp"
#include <stdexcept>

using namespace markov::trace_graph;
using namespace markov::trace_graph::modules::hicache;
namespace {
void require(bool value, const char * message) {
    if (!value) throw std::runtime_error(message);
}
core::DagGraph fixture() {
    std::vector<core::TraceEvent> events(11);
    const uint64_t durations[]{ 1, 4, 1, 12, 2, 0, 8, 0, 8, 1, 20 };
    for (size_t i = 0; i < events.size(); ++i) {
        events[i].name = "layer test";
        events[i].pid = "1";
        events[i].tid = "main";
        events[i].dur = durations[i];
    }
    events[1].set_arg("size(B)", "100");
    events[3].set_arg("size(B)", "300");
    core::DagGraph graph(std::move(events), 0);
    for (size_t i = 0; i < 11; ++i) graph.add_node(i, i == 0 || i == 9, i >= 1 && i <= 4 ? "H2D" : "compute");
    for (size_t i = 1; i < 4; ++i) graph.add_edge(i, i + 1, core::DagEdgeKind::Stream);
    graph.add_edge(2, 5, core::DagEdgeKind::Sync);
    graph.add_edge(4, 7, core::DagEdgeKind::Sync);
    for (size_t i = 5; i < 8; ++i) graph.add_edge(i, i + 1, core::DagEdgeKind::Stream);
    graph.add_edge(0, 9, core::DagEdgeKind::Sequential);
    core::TraceEvent fact;
    fact.name = "lookup";
    fact.pid = "1";
    fact.tid = "main";
    fact.source_channel = core::TraceSourceChannel::PythonProbe;
    fact.set_arg("fact", R"({"class":"workload_identity","role":"cache_lookup_input","consumers":["hicache_dag_patch"]})");
    graph.set_hicache_fact_events({ fact });
    return graph;
}
} // namespace

void check_hicache_layer_io() {
    {
        auto immediate = fixture();
        core::DagMutationPlan redirect{ .component = "hicache" };
        for (size_t i = 0; i < 128; ++i)
            redirect.synthetic_nodes.push_back({ .synthetic_id = "ready:" + std::to_string(i), .node = { .name = "ready" }, .effect_id = "ready" });
        for (size_t id = 0; id < immediate.edge_count(); ++id)
            if (immediate.edge(id).src == 0 && immediate.edge(id).dst == 9)
                redirect.redirect_edges.push_back({ .edge_index = id, .dst = core::DagNodeRef::synthetic("ready:127"), .effect_id = "ready" });
        redirect.add_edges.push_back({ .src = core::DagNodeRef::synthetic("ready:127"),
                                       .dst = core::DagNodeRef::existing(9),
                                       .kind = core::DagEdgeKind::Mutation,
                                       .effect_id = "ready" });
        redirect.disable_nodes.push_back(10);
        const auto mutation = core::apply_dag_mutation_plan(immediate, redirect);
        const auto validation = patch::validate_hicache_applied_patch(immediate, {}, {}, redirect, mutation, true);
        require(validation.plan_journal_exact && validation.prospective_materialization_exact,
                "removing an unrelated old node must not erase a redirected ingress to a new node");
    }
    auto graph = fixture();
    const patch::HiCacheSourceDagIndex source(graph);
    HiCacheLayerWaitObservation waits;
    for (size_t i = 0; i < 2; ++i) {
        HiCacheLayerWaitCall call;
        call.enabled = true;
        call.request_id = "donor";
        call.phase = "EXTEND";
        call.layer = i;
        call.before = 0;
        call.record = 2 + 2 * i;
        waits.calls.push_back(call);
    }
    patch::HiCacheRewriteDecision donor;
    donor.request_id = "donor";
    donor.source_readiness_topology_reused = true;
    donor.source_fact_node_id = source.fact_nodes().front().node_id;
    donor.owned_duration_nodes = { 1, 3 };
    const auto layers = patch::observe_hicache_layer_transfers(source, waits, donor);
    require(layers.size() == 2 && layers[0].bytes == 100 && layers[1].bytes == 300 && layers[0].record_us == 1 && layers[1].record_us == 2,
            "layout is read from source transfer bytes and Record dependencies");
    auto ambiguous = waits;
    ambiguous.calls[1].record = 2;
    require(patch::observe_hicache_layer_transfers(source, ambiguous, donor).empty(), "one Record cannot invent distinct layer readiness");
    auto incomplete = donor;
    incomplete.owned_duration_nodes = { 1 };
    require(patch::observe_hicache_layer_transfers(source, waits, incomplete).empty(), "the Record chain cannot hide an unowned transfer");

    auto all_first = fixture();
    for (size_t id = 0; id < all_first.edge_count(); ++id) {
        const auto & link = all_first.edge(id);
        if (link.kind == core::DagEdgeKind::Stream && link.src < 4) all_first.disable_edge(id);
    }
    all_first.add_edge(1, 3, core::DagEdgeKind::Stream);
    all_first.add_edge(3, 2, core::DagEdgeKind::Stream);
    all_first.add_edge(2, 4, core::DagEdgeKind::Stream);
    const patch::HiCacheSourceDagIndex all_first_source(all_first);
    const auto bundled = patch::observe_hicache_layer_transfers(all_first_source, waits, donor);
    require(bundled.size() == 2 && bundled[0].bytes == 400 && bundled[1].bytes == 0,
            "all-layer transfer before the first Record must not become fictional per-layer DMA");

    patch::HiCacheRewriteDecision target;
    target.effect_id = "new_load";
    target.synthetic_id = "new_load";
    target.request_id = "request";
    target.rewrite_kind = patch::HiCacheRewriteKind::InsertIo;
    target.shadow_plan_ready = true;
    target.target_effect_state = model::HiCacheTargetEffectState::Required;
    target.duration_us = 101;
    target.resource_lane = "H2D";
    target.source_execution_anchor_node_id = 0;
    target.consumer_dependency_required = true;
    target.consumer_anchors = { 9 };
    target.completion_join_required = true;
    target.completion_join_uses_service = true;
    target.completion_join_synthetic_id = "join";
    target.control_ready_anchor_node_id = 0;
    target.wait_exit_anchor_node_id = 9;
    core::DagMutationPlan plan{ .component = "hicache" };
    plan.synthetic_nodes.push_back({
        .synthetic_id = "new_load",
        .node = { .name = "new load", .is_cpu = false, .lane_key = "H2D", .duration = 101 },
        .effect_id = "new_load"
    });
    plan.synthetic_nodes.push_back({
        .synthetic_id = "join",
        .node = { .name = "join", .is_cpu = false, .lane_key = "hicache_completion_join" },
        .effect_id = "new_load"
    });
    using Ref = core::DagNodeRef;
    const auto edge = [&](Ref from, Ref to, core::DagEdgeKind kind = core::DagEdgeKind::Mutation, const char * effect = "new_load") {
        plan.add_edges.push_back({ .src = from, .dst = to, .kind = kind, .effect_id = effect });
    };
    edge(Ref::existing(0), Ref::synthetic("new_load"));
    edge(Ref::synthetic("new_load"), Ref::synthetic("join"));
    edge(Ref::existing(0), Ref::synthetic("join"));
    edge(Ref::synthetic("join"), Ref::existing(9));
    edge(Ref::synthetic("new_load"), Ref::existing(10), core::DagEdgeKind::Mutation, "H2D");
    auto bundled_plan = plan;
    auto bundled_target = target;
    require(patch::expand_hicache_layer_io(bundled, bundled_target, bundled_plan) && bundled_target.layer_io[0].service_us == 101
                && bundled_target.layer_io[1].service_us == 0,
            "all-layer backend spends the full transfer cost before the first readiness event");
    require(patch::expand_hicache_layer_io(layers, target, plan), "source layout expands the predicted transfer");
    require(target.layer_io[0].service_us == 25 && target.layer_io[1].service_us == 76,
            "byte-weighted integer allocation conserves all 101 us, including rounding");
    for (size_t i = 0; i < 2; ++i) {
        const auto wait = 5 + 2 * i;
        target.layer_io[i].waits.push_back(Ref::existing(wait));
        edge(Ref::synthetic(target.layer_io[i].ready_id), Ref::existing(wait), core::DagEdgeKind::Sync, "hicache_layer_wait");
        for (const auto id : source.incoming_edge_ids(wait))
            if (graph.edge(id).kind == core::DagEdgeKind::Sync) plan.disable_edges.push_back(id);
    }
    const auto mutation = core::apply_dag_mutation_plan(graph, plan);
    patch::HiCacheShadowRewriteTransaction shadow;
    patch::HiCacheRewriteDecision next;
    next.effect_id = "next_load";
    next.rewrite_kind = patch::HiCacheRewriteKind::NoOp;
    next.shadow_plan_ready = true;
    next.source_readiness_topology_reused = true;
    next.owned_duration_nodes = { 10 };
    shadow.decisions = { target, next };
    shadow.topology_valid = true;
    patch::HiCacheIoResourcePlan resources;
    resources.lane_dependencies.push_back({ .resource_lane = "H2D", .predecessor_effect_id = "new_load", .successor_effect_id = "next_load" });
    const auto check = [&] { return patch::validate_hicache_applied_patch(graph, shadow, resources, plan, mutation, true); };
    const auto valid = check();
    require(valid.status == "ready", "layer costs, ingress, consumers and resource ordering validate after materialization");
    const auto added =
        std::ranges::find_if(mutation.journal.records, [](const auto & record) { return record.action == core::DagMutationAction::AddSyntheticNode; });
    require(added != mutation.journal.records.end(), "fixture includes synthetic-node journal entries");
    const auto at = static_cast<size_t>(added - mutation.journal.records.begin());
    auto duplicated = mutation;
    duplicated.journal.records.push_back(*added);
    require(!patch::validate_hicache_applied_patch(graph, shadow, resources, plan, duplicated, true).plan_journal_exact,
            "synthetic identity index must reject duplicate matching records");
    auto missing = mutation;
    missing.journal.records.erase(missing.journal.records.begin() + at);
    require(!patch::validate_hicache_applied_patch(graph, shadow, resources, plan, missing, true).plan_journal_exact,
            "a materialized synthetic node still requires its journal entry");
    auto wrong_effect = mutation;
    wrong_effect.journal.records[at].effect_id = "unrelated";
    require(!patch::validate_hicache_applied_patch(graph, shadow, resources, plan, wrong_effect, true).plan_journal_exact,
            "the right synthetic identity cannot substitute for the wrong effect");
    auto premature = graph;
    const auto finish = mutation.synthetic_node_ids.at(target.layer_io.back().ready_id);
    for (size_t id = 0; id < premature.edge_count(); ++id)
        if (premature.edge(id).src == finish && premature.edge(id).dst == 10) premature.disable_edge(id);
    premature.add_edge(mutation.synthetic_node_ids.at(target.synthetic_id), 10, core::DagEdgeKind::Mutation);
    require(!patch::validate_hicache_applied_patch(premature, shadow, resources, plan, mutation, true).lane_dependencies_exact,
            "a resource successor cannot be released by the zero-cost aggregate start");
    (void)simulation::run_topological_simulation(graph);
    require(graph.node(6).completion_time == 35 && graph.node(8).completion_time == 113,
            "first-layer computation overlaps later transfers rather than waiting for the whole load");
    require(graph.node(9).completion_time == 2 && graph.node(10).completion_time == 125,
            "CPU submission does not wait for DMA, but subsequent I/O waits for the final Record");
    const auto first = mutation.synthetic_node_ids.at(target.layer_io[0].service_id);
    graph.set_node_duration(first, 26);
    require(!check().records.front().synthetic_cost_exact, "changed layer cost cannot pass as conserved prediction");
    graph.set_node_duration(first, 25);
    graph.add_edge(2, 5, core::DagEdgeKind::Sync);
    require(!check().records.front().consumer_dependency_exact, "retaining the old Record alongside the predicted one is rejected");
}
