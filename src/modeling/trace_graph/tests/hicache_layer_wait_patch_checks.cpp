#include "markov/trace_graph/modules/hicache/patch/applied_validator.hpp"
#include "markov/trace_graph/modules/hicache/patch/layer_wait_patch.hpp"
#include "markov/trace_graph/modules/hicache/runtime/layer_calls.hpp"
#include "markov/trace_graph/simulation/topological_simulator.hpp"
#include <stdexcept>

using namespace markov::trace_graph;
using namespace markov::trace_graph::modules::hicache;
namespace {
void require(bool value, const char * message) {
    if (!value) throw std::runtime_error(message);
}
core::TraceEvent event(const char * name, uint64_t ts, uint64_t dur, const char * tid = "main") {
    core::TraceEvent e;
    e.name = name;
    e.pid = "1";
    e.tid = tid;
    e.ts = ts;
    e.dur = dur;
    e.cat = "cpu_op";
    e.source_channel = core::TraceSourceChannel::Torch;
    return e;
}
core::DagGraph fixture() {
    std::vector<core::TraceEvent> events{ event("before", 0, 5),    event("enqueue0", 10, 2), event("enqueue1", 25, 2),
                                          event("enqueue2", 40, 2), event("after", 50, 2),    event("end", 70, 2) };
    for (size_t i = 0; i < 3; ++i) {
        events[i + 1].cat = "enqueue";
        events[i + 1].set_arg("correlation_id", std::to_string(i));
        auto worker = event("AscendCL@aclrtStreamWaitEvent", 11 + 15 * i, 1, "worker");
        worker.set_arg("correlation_id", std::to_string(i));
        events.push_back(worker);
    }
    events.push_back(event("EVENT_RECORD", 0, 100, "H2D"));
    events.push_back(event("earlier device work", 5, 2, "compute"));
    for (size_t i = 0; i < 3; ++i) events.push_back(event("logical_event_wait", 12 + 15 * i, 0, "compute"));
    events.push_back(event("kernel", 53, 5, "compute"));
    events.push_back(event("sync", 60, 0, "sync"));
    core::DagGraph graph(std::move(events), 0);
    for (size_t i = 0; i < graph.events().size(); ++i) graph.add_node(i, i < 9 || i == 15, graph.events()[i].tid);
    for (size_t i = 1; i < 6; ++i) {
        graph.add_edge(i - 1, i, core::DagEdgeKind::Sequential);
        const auto & previous = graph.event_for_node(i - 1);
        graph.mutable_node(i - 1).original_cpu_gap_after = graph.mutable_node(i - 1).cpu_gap_after = graph.event_for_node(i).ts - previous.ts - previous.dur;
    }
    for (size_t i = 0; i < 3; ++i) {
        graph.add_edge(i + 1, i + 6, core::DagEdgeKind::Correlation);
        graph.add_edge(i + 6, i + 11, core::DagEdgeKind::Correlation);
        graph.add_edge(9, i + 11, core::DagEdgeKind::Sync);
        graph.add_edge(i + 10, i + 11, core::DagEdgeKind::Stream);
        if (i) graph.add_edge(i + 5, i + 6, core::DagEdgeKind::Sequential);
    }
    graph.add_edge(13, 14, core::DagEdgeKind::Stream);
    graph.add_edge(13, 15, core::DagEdgeKind::Sync);
    auto batch = event("runtime.hicache.layer_waits", 7, 43);
    batch.set_arg("wait_clock", "profiler_ns");
    batch.set_arg("status", "returned");
    batch.set_arg("request_ids", "[\"request\"]");
    batch.set_arg("phase", "EXTEND");
    batch.set_arg("consumer_index", "0");
    batch.set_arg("layer_count", "3");
    batch.set_arg("wait_intervals", "[[0,8000,16000],[1,23000,31000],[2,38000,46000]]");
    auto inactive = batch;
    inactive.ts = 60;
    inactive.dur = 1;
    inactive.set_arg("consumer_index", "-1");
    inactive.set_arg("request_ids", "[\"calibration\"]");
    inactive.set_arg("wait_intervals", "[[0,60100,60600]]");
    graph.set_runtime_observations({ batch, inactive });
    auto fact = event("lookup", 0, 0);
    fact.source_channel = core::TraceSourceChannel::PythonProbe;
    fact.set_arg("fact", R"({"class":"workload_identity","role":"cache_lookup_input","consumers":["hicache_dag_patch"]})");
    graph.set_hicache_fact_events({ fact });
    return graph;
}
} // namespace

void check_hicache_layer_wait_patch() {
    {
        HiCacheFactParser parser;
        auto timing = event("runtime.hicache.layer_waits", 5, 1);
        timing.cat = "runtime_diagnostic";
        parser.observe_token_dictionaries(timing);
        timing.cat = "hicache";
        bool rejected = false;
        try { parser.observe_token_dictionaries(timing); }
        catch (const std::invalid_argument &) { rejected = true; }
        require(rejected, "timing-only context is ignored without weakening malformed state-fact checks");
    }
    for (const bool identified : {false, true}) for (const bool before_window : {false, true}) {
        auto graph = fixture();
        const auto submission = graph.add_synthetic_node({ .name = "other enqueue", .lane_key = std::string(graph.node_lane_key(1)),
            .observed_point = core::DagObservedPoint{"1", "main", 0, 0} });
        const auto worker = graph.add_synthetic_node({ .name = "other worker", .lane_key = std::string(graph.node_lane_key(6)),
            .cpu_task_ready_delay_us = 0, .observed_point = core::DagObservedPoint{"1", "worker", 0, 0} });
        graph.mutable_node(submission).gpu_id = graph.mutable_node(worker).gpu_id = 0;
        graph.add_edge(submission, worker, core::DagEdgeKind::Correlation);
        auto observations = graph.runtime_observations();
        if (before_window) {
            observations.back().ts = 5;
            observations.back().set_arg("wait_intervals", "[[0,5100,5600]]");
        }
        auto facts = graph.hicache_fact_events();
        facts.front().set_arg("request_id", identified ? "calibration" : "unrelated");
        graph.set_prelude_context_events({facts.front(), observations.back()});
        observations.pop_back();
        graph.set_runtime_observations(std::move(observations));
        runtime::HiCacheWriteStreamInsertions insertions;
        runtime::HiCacheLayerCalls calls(insertions);
        calls.bind(graph, 7, 50);
        const std::vector<size_t> empty;
        bool missing = false;
        try {
            (void)simulation::run_topological_simulation(graph, {}, [&](size_t node, uint64_t, simulation::FutureDag & future) {
                calls.advance(node, [&](const std::string &, uint64_t) -> const std::vector<size_t> & { return empty; }, future);
            });
        } catch (const std::runtime_error & error) {
            if (std::string(error.what()) != "Base lacks an inactive layer-call return sample") throw;
            missing = true;
        }
        const bool eligible = identified && before_window;
        require(missing != eligible, "only identified requests before the window supply missing inactive costs");
        require(calls.prepared_calls() == 3, "preparation samples must not expand the formal execution window");
        if (eligible) require(calls.inactive_calls() == 3, "three formal calls use the observed preparation return cost");
    }
    for (const bool active : { false, true }) for (const bool added : { false, true }) for (const bool grouped : { false, true })
    for (const bool measured : { false, true }) {
        auto dynamic = fixture();
        if (measured) dynamic.cpu_service_cost().add({"1","main"},{52,70,9});
        // A longer preparation sample must not override the formal 500 ns
        // return sample; the exact expected times below check that precedence.
        auto preparation = dynamic.runtime_observations().back();
        preparation.ts = 5;
        preparation.set_arg("wait_intervals", "[[0,5100,5900]]");
        auto identity = dynamic.hicache_fact_events().front();
        identity.set_arg("request_id", "calibration");
        dynamic.set_prelude_context_events({identity, preparation});
        if (grouped) {
            auto observations = dynamic.runtime_observations();
            observations.back().dur = 2;
            observations.back().set_arg("wait_intervals", "[[0,60100,60600],[1,60700,61200]]");
            dynamic.set_runtime_observations(std::move(observations));
        }
        // The older static fixture intentionally omitted host submission for
        // these consumers. Dynamic insertion needs the real causal ordering:
        // subsequent compute/synchronization cannot start before submission.
        dynamic.add_edge(4, 14, core::DagEdgeKind::Correlation);
        dynamic.add_edge(4, 15, core::DagEdgeKind::Sync);
        const auto later = dynamic.add_synthetic_node({ .name = "compute after inactive call", .is_cpu = false,
            .lane_key = std::string(dynamic.node_lane_key(14)), .duration = 1,
            .observed_point = core::DagObservedPoint{"1", "compute", 75, 0} });
        dynamic.add_edge(5, later, core::DagEdgeKind::Correlation);
        dynamic.add_edge(14, later, core::DagEdgeKind::Stream);
        const auto submission = dynamic.add_synthetic_node({ .name = "other enqueue", .lane_key = std::string(dynamic.node_lane_key(1)),
                                                             .observed_point = core::DagObservedPoint{ "1", "main", 0, 0 } });
        const auto worker = dynamic.add_synthetic_node({ .name = "other worker", .lane_key = std::string(dynamic.node_lane_key(6)),
                                                         .cpu_task_ready_delay_us = 0,
                                                         .observed_point = core::DagObservedPoint{ "1", "worker", 0, 0 } });
        dynamic.mutable_node(submission).gpu_id = dynamic.mutable_node(worker).gpu_id = 0;
        dynamic.add_edge(submission, worker, core::DagEdgeKind::Correlation);
        runtime::HiCacheWriteStreamInsertions insertions;
        runtime::HiCacheLayerCalls calls(insertions);
        calls.bind(dynamic, 7, 100);
        const std::vector<size_t> records = active ? std::vector<size_t>{ 9, 9, 9 } : std::vector<size_t>{};
        const std::vector<size_t> added_records = added ? std::vector<size_t>{9, 9} : std::vector<size_t>{};
        const runtime::HiCacheLayerCalls::Consumer consumer = [&](const std::string & pid, uint64_t batch) -> const std::vector<size_t> & {
            require(pid == "1" && (batch == 7000 || batch == 60000), "layer calls use their own observed forward identity");
            return batch == 7000 ? records : added_records;
        };
        const auto execution = simulation::run_topological_simulation(dynamic, {}, [&](size_t id, uint64_t, simulation::FutureDag & future) {
            calls.advance(id, consumer, future);
        });
        const size_t source_inactive = grouped ? 2 : 1;
        const size_t new_active = added ? source_inactive : 0;
        require(calls.prepared_calls() == 3 + source_inactive && calls.active_calls() == (active ? 3 : 0) + new_active
                && calls.inactive_calls() == (active ? 0 : 3) + source_inactive - new_active,
                "each active or inactive source call must execute exactly one target branch");
        require(execution.cpu_task_count == (active ? 4 : 1) + new_active, "inactive calls must not leave zero-cost worker tasks in the FIFO");
        require(dynamic.node(5).completion_time == (active ? 72 : 49) + (added ? (grouped ? 15 : 8) : 0)
                - (measured ? 9 : 0) + (measured && added && grouped ? 1 : 0),
                "grouped submicrosecond calls conserve the original gap and replace only their own duration");
        require(dynamic.node(14).completion_time == (active ? 105 : 34), "only active target layer waits retain the load readiness dependency");
        if (added) require(dynamic.node(later).simulation_start >= dynamic.node(9).completion_time,
                           "a newly active call must hold later compute until target data is ready");
        const auto saved = dynamic.nodes();
        (void)simulation::run_topological_simulation(dynamic);
        for (const auto & node : saved)
            require(node.simulation_start == dynamic.node(node.id).simulation_start && node.completion_time == dynamic.node(node.id).completion_time,
                    "dynamic layer call replacement survives static replay");
    }
    {
        auto detached = fixture();
        const patch::HiCacheSourceDagIndex source(detached);
        const auto waits = observe_hicache_layer_waits(source);
        core::DagMutationPlan neutral{ .component = "layer_wait_runtime" };
        const std::vector<patch::HiCacheLayerWaitRemoval> duplicate{ { &waits.calls[0], 0 }, { &waits.calls[0], 0 } };
        require(patch::append_hicache_layer_wait_removals(source, duplicate, neutral).status == "blocked" && neutral.empty(),
                "duplicate removal must not subtract the same CPU gap twice or leave a partial plan");
        const std::vector<patch::HiCacheLayerWaitRemoval> incomplete{ { nullptr, 0 } };
        require(patch::append_hicache_layer_wait_removals(source, incomplete, neutral).status == "blocked" && neutral.empty(),
                "missing call ownership must leave the existing plan unchanged");
        std::vector<patch::HiCacheLayerWaitRemoval> changes;
        for (const auto & call : waits.calls)
            if (call.enabled) changes.push_back({ &call, 0 });
        const auto removed = patch::append_hicache_layer_wait_removals(source, changes, neutral);
        require(removed.status == "ready" && removed.removed_calls == 3 && removed.added_return_us == 0,
                "dynamic preparation can preserve zero-cost submission anchors without inventing a target state");
        (void)core::apply_dag_mutation_plan(detached, neutral);
        (void)simulation::run_topological_simulation(detached);
        require(detached.node(5).completion_time == 48 && detached.node(14).completion_time == 7,
                "neutral call sites retain outside gaps and device order without obsolete event readiness");
        for (const auto id : { 1, 2, 3 })
            require(detached.node(id).active && detached.node(id).duration == 0, "call anchors remain available for dynamic expansion");
    }
    {
        auto measured=fixture();
        measured.cpu_service_cost().add({"1","main"},{5,10,2});
        measured.cpu_service_cost().add({"1","main"},{12,25,6});
        measured.cpu_service_cost().add({"1","main"},{27,40,6});
        measured.cpu_service_cost().add({"1","main"},{42,50,4});
        const patch::HiCacheSourceDagIndex source(measured);
        const auto waits=observe_hicache_layer_waits(source);
        std::vector<patch::HiCacheLayerWaitRemoval> changes;
        for (const auto& call : waits.calls) if (call.enabled) changes.push_back({&call,0});
        core::DagMutationPlan removal{.component="layer_wait_runtime"};
        require(patch::append_hicache_layer_wait_removals(source,changes,removal).status=="ready",
                "layer removal must retain original gap coordinates with service measurements");
        (void)core::apply_dag_mutation_plan(measured,removal);
        require(measured.cpu_service_gap_duration(0)==1 && measured.cpu_service_gap_duration(1)==4
                && measured.cpu_service_gap_duration(2)==4 && measured.cpu_service_gap_duration(3)==2,
                "multiple layer removals retain only the outside service fragments");
        (void)simulation::run_topological_simulation(measured);
        require(measured.node(5).completion_time==38,"layer removal replay conserves retained normal service");
    }
    auto graph = fixture();
    const patch::HiCacheSourceDagIndex source(graph);
    const auto waits = observe_hicache_layer_waits(source);
    require(waits.status == "ready" && waits.calls.size() == 4, "fixture must supply complete source call ownership");
    patch::HiCacheRewriteDecision off;
    off.effect_type = model::HiCacheEffectType::Loadback;
    off.target_effect_state = model::HiCacheTargetEffectState::NotRequired;
    off.request_id = "request";
    off.source_fact_node_id = source.fact_nodes().front().node_id;
    auto calibration = off;
    calibration.request_id = "calibration";
    auto same = off;
    same.target_effect_state = model::HiCacheTargetEffectState::Required;
    same.source_readiness_topology_reused = true;
    core::DagMutationPlan plan{ .component = "hicache" };
    std::vector<patch::HiCacheRewriteDecision> decisions{ same, calibration };
    const auto retained = patch::append_hicache_layer_wait_plan(source, waits, decisions, plan);
    require(retained.status == "ready" && retained.retained_calls == 4 && plan.empty(), "same state must leave graph and costs unchanged");

    auto admission = same;
    admission.eligibility_timestamp_us = 1;
    decisions = { off, admission, calibration };
    const auto repeated = patch::append_hicache_layer_wait_plan(source, waits, decisions, plan);
    require(repeated.status == "ready" && repeated.retained_calls == 4 && plan.empty(),
            "discovery without loadback must not hide a later admission loadback for the same request");

    auto later = off;
    later.eligibility_timestamp_us = 30;
    auto batches = waits;
    batches.calls[1].phase = "DECODE";
    batches.calls[1].batch_start_ns = 20'000;
    batches.calls[2].phase = "DECODE";
    batches.calls[2].batch_start_ns = 35'000;
    decisions = { admission, off, calibration };
    require(patch::append_hicache_layer_wait_plan(source, batches, decisions, plan).retained_calls == 4 && plan.empty(),
            "Decode inherits admission state; decision iteration order is irrelevant");

    auto orphan = batches;
    orphan.calls[0].phase = "DECODE";
    decisions = { off, calibration };
    require(patch::append_hicache_layer_wait_plan(source, orphan, decisions, plan).status == "blocked" && plan.empty(),
            "Decode without an observed Extend cannot invent its consumer state");

    decisions = { off, admission, later, calibration };
    require(patch::append_hicache_layer_wait_plan(source, batches, decisions, plan).status == "blocked" && plan.empty(),
            "a new loadback opportunity during Decode needs a scheduling boundary");

    batches.calls[2].phase = "EXTEND";
    core::DagMutationPlan next_batch{ .component = "hicache" };
    decisions = { later, off, calibration, admission };
    const auto reset = patch::append_hicache_layer_wait_plan(source, batches, decisions, next_batch);
    require(reset.status == "ready" && reset.retained_calls == 3 && reset.removed_calls == 1,
            "a later Extend drains its own queue; its no-loadback decision cannot change earlier calls");

    auto future = same;
    future.eligibility_timestamp_us = 100;
    core::DagMutationPlan before_future{ .component = "hicache" };
    decisions = { future, off, calibration };
    const auto prior = patch::append_hicache_layer_wait_plan(source, waits, decisions, before_future);
    require(prior.status == "ready" && prior.removed_calls == 3, "future loadback cannot enable waits in an earlier batch");

    auto needs_insert = calibration;
    needs_insert.target_effect_state = model::HiCacheTargetEffectState::Required;
    decisions = { off, needs_insert };
    const auto incomplete = patch::append_hicache_layer_wait_plan(source, waits, decisions, plan);
    require(incomplete.status == "blocked" && incomplete.required_insertions == 1 && plan.empty(),
            "incomplete insertion cannot silently apply only the removal half");

    plan.set_node_e2e_eligibility.push_back({ .node_id = 11, .counts_toward_e2e = false });
    decisions = { off, calibration };
    const auto changed = patch::append_hicache_layer_wait_plan(source, waits, decisions, plan);
    require(changed.status == "ready" && changed.removed_calls == 3 && changed.added_return_us == 1 && changed.removed_main_cpu_us == 24
                && changed.removed_worker_us == 3,
            "three 500 ns returns are accumulated; only active call service and internal gaps are replaced");
    require(plan.set_node_e2e_eligibility.empty(), "deleted infrastructure must not receive a conflicting eligibility update");
    const auto mutation = core::apply_dag_mutation_plan(graph, plan);
    const auto validation = patch::validate_hicache_applied_patch(graph, {}, {}, plan, mutation, true);
    require(validation.plan_journal_exact, "deletion must be proven by materialized graph and journal");
    auto corrupt = mutation;
    std::erase_if(corrupt.journal.records, [](const auto & record) { return record.action == core::DagMutationAction::DisableNode; });
    require(!patch::validate_hicache_applied_patch(graph, {}, {}, plan, corrupt, true).plan_journal_exact,
            "a missing deletion journal cannot pass endpoint validation");
    (void)simulation::run_topological_simulation(graph);
    require(graph.node(5).completion_time == 49, "outside-call gaps and fast returns remain on the main thread");
    require(graph.node(14).completion_time == 7 && graph.node(15).completion_time == 2,
            "stream work and synchronization survive, without the old Record dependency");
    for (const auto id : { 6, 7, 8, 11, 12, 13 }) require(!graph.node(id).active, "obsolete worker and device waits are disabled");
    graph.set_node_duration(9, 1'000);
    (void)simulation::run_topological_simulation(graph);
    require(graph.node(14).completion_time == 7, "obsolete loadback readiness no longer delays the consumer");
}
