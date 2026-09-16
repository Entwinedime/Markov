/** @file Exact call ownership, independent of target costs. */
#include "markov/trace_graph/modules/hicache/layer_waits.hpp"
#include "markov/trace_graph/modules/hicache/phase_observation.hpp"
#include "markov/trace_graph/modules/hicache/patch/io_operation_ledger.hpp"
#include "markov/trace_graph/core/dag_builder.hpp"
#include "markov/trace_graph/simulation/topological_simulator.hpp"
#include <algorithm>
#include <stdexcept>

using namespace markov::trace_graph;
using namespace markov::trace_graph::modules::hicache;
namespace {
void require(bool value, const char * message) { if (!value) throw std::runtime_error(message); }

core::TraceEvent event(const char * name, uint64_t ts, uint64_t dur, const char * tid = "1") {
    core::TraceEvent e;
    e.name = name; e.pid = "1"; e.tid = tid; e.ts = ts; e.dur = dur; e.cat = "cpu_op";
    e.source_channel = core::TraceSourceChannel::Torch;
    return e;
}

core::DagGraph fixture() {
    std::vector<core::TraceEvent> events{
        event("before", 10, 5), event("Enqueue@wait_event", 25, 5), event("after", 50, 5),
        event("Enqueue@compute", 60, 5), event("AscendCL@aclrtStreamWaitEvent", 31, 2, "2"),
        event("EVENT_RECORD", 15, 0, "3"), event("EVENT_WAIT", 33, 0, "4"),
        event("prefill kernel", 70, 10, "4"), event("decode next", 220, 1),
        event("decode before", 220, 0), event("Enqueue@decode", 230, 2), event("decode kernel", 240, 10, "4")};
    for (const auto id : {1, 3, 10}) events[id].cat = "enqueue";
    events[8].ts_submicro_ns = 600;
    events[9].dur_submicro_ns = 80;
    core::DagGraph graph(std::move(events), 0);
    for (size_t i = 0; i < graph.events().size(); ++i) {
        const bool cpu = i < 5 || (i >= 8 && i <= 10);
        graph.add_node(i, cpu, cpu ? (i == 4 ? "CPU:1:2" : "CPU:1:1") : (i == 5 ? "io" : "compute"));
    }
    graph.add_edge(1, 4, core::DagEdgeKind::Correlation);
    graph.add_edge(4, 6, core::DagEdgeKind::Correlation);
    graph.add_edge(5, 6, core::DagEdgeKind::Sync);
    graph.add_edge(3, 7, core::DagEdgeKind::Correlation);
    graph.add_edge(10, 11, core::DagEdgeKind::Correlation);
    graph.add_edge(6, 7, core::DagEdgeKind::Stream);
    graph.mutable_node(6).submit_ts = 31;
    graph.mutable_node(7).submit_ts = 60;
    graph.mutable_node(11).submit_ts = 230;
    const std::vector<size_t> cpu{0, 1, 2, 3, 9, 8, 10};
    for (size_t i = 1; i < cpu.size(); ++i) {
        const auto before = cpu[i - 1], after = cpu[i];
        const auto & e = graph.event_for_node(before);
        graph.mutable_node(before).cpu_gap_after = graph.event_for_node(after).ts - e.ts - e.dur;
        graph.add_edge(before, after, core::DagEdgeKind::Sequential);
    }
    auto active = event("runtime.hicache.layer_waits", 10, 90);
    active.source_channel = core::TraceSourceChannel::PythonProbe;
    active.cat = "runtime_diagnostic";
    active.set_arg("wait_clock", "profiler_ns"); active.set_arg("status", "returned");
    active.set_arg("request_ids", R"(["request"])"); active.set_arg("phase", "EXTEND");
    active.set_arg("consumer_index", "0"); active.set_arg("layer_count", "1");
    active.set_arg("wait_intervals", "[[0,20000,40000]]");
    auto inactive = active;
    inactive.ts = 200; inactive.set_arg("phase", "DECODE"); inactive.set_arg("consumer_index", "-1");
    inactive.set_arg("wait_intervals", "[[0,220100,220400]]");
    graph.set_runtime_observations({active, inactive});
    auto fact = event("extend", 1, 1);
    fact.source_channel = core::TraceSourceChannel::PythonProbe;
    fact.set_arg("fact", R"({"class":"workload_identity","role":"cache_extend_input","consumers":["hicache_dag_patch"]})");
    fact.set_arg("phase", "start"); fact.set_arg("request_ids", R"(["request"])");
    fact.set_arg("token_counts", "[16]"); fact.set_arg("batch_size", "1"); fact.set_arg("source_page_size", "128");
    graph.set_hicache_fact_events({fact});
    graph.set_phase_marker_events({{0, event("step[EXTEND toks=16]", 10, 90)}, {0, event("step[DECODE]", 200, 90)}});
    return graph;
}
} // namespace

void check_hicache_layer_waits() {
    auto child = event("child", 10, 0);
    child.ts_submicro_ns = 500; child.dur_submicro_ns = 100;
    const auto control = core::DagBuilder(1).build({event("hicache.control.check", 10, 10), child, event("next", 30, 1)}, 0);
    const auto gap = patch::HiCacheSourceDagIndex(control).timing_interval_ownership("1", "1", 20, 5);
    require(gap.status == "ready" && gap.owned_gap_slices.size() == 1
            && control.node(gap.owned_gap_slices.front().owner_node_id).cpu_gap_after >= 5,
            "precise call indexing must not reorder the materialized self-time gap owner");
    auto graph = fixture();
    const auto original = simulation::run_topological_simulation(graph).e2e_us;
    const auto waits = observe_hicache_layer_waits(patch::HiCacheSourceDagIndex(graph));
    require(waits.status == "ready" && waits.calls.size() == 2, "active and inactive call sites must both be observed");
    require(waits.cpu_node_ids == std::unordered_set<size_t>{1, 4} && waits.device_wait_node_ids == std::unordered_set<size_t>{6},
            "only the proven wait submission and worker are HiCache CPU work");
    require(waits.calls[0].record == 5 && waits.calls[0].before == 0 && waits.calls[0].after == 2,
            "active call retains its own layer Record and CPU boundaries");
    require(waits.calls[1].before == 9 && waits.calls[1].after == 8 && waits.calls[1].end_ns - waits.calls[1].start_ns == 300,
            "a sub-microsecond no-op remains between precise leaves sharing the same integer timestamp");
    const HiCacheLayerWaitObservation unclassified;
    const auto old_phase = observe_hicache_phases(graph, &unclassified);
    const auto phase = observe_hicache_phases(graph, &waits);
    require(old_phase.status == "ready" && phase.status == "ready", "fixture must exercise complete phase ownership");
    require(old_phase.phase_owned_submit_cpu_node_count == phase.phase_owned_submit_cpu_node_count + 2,
            "phase must not retain the HiCache enqueue or its correlated worker");
    (void)mark_observed_hicache_scope(graph);
    require(graph.scope_node_owned(1) && graph.scope_node_owned(4) && graph.scope_node_owned(6),
            "moving calls out of phase must preserve their HiCache scope ownership");
    require(graph.scope_gap_duration(0) == 5 && graph.scope_gap_duration(1) == 10,
            "only gaps inside the exact call envelope belong to the HiCache call");
    require(simulation::run_topological_simulation(graph).e2e_us == original, "ownership must not alter full timing");

    graph.disable_edge(2);
    const auto missing = observe_hicache_layer_waits(patch::HiCacheSourceDagIndex(graph));
    require(missing.status == "partial" && missing.issues.contains("nonunique_layer_record") && missing.cpu_node_ids.empty(),
            "missing readiness cannot authorize node removal or phase exclusion");
    graph.add_edge(5, 6, core::DagEdgeKind::Sync);
    graph.add_edge(7, 6, core::DagEdgeKind::Sync);
    const auto ambiguous = observe_hicache_layer_waits(patch::HiCacheSourceDagIndex(graph));
    require(ambiguous.issues.contains("nonunique_layer_record"), "two Record predecessors cannot be guessed");

    auto other_lane = fixture();
    auto records = other_lane.runtime_observations();
    records[0].tid = "unobserved";
    other_lane.set_runtime_observations(std::move(records));
    require(observe_hicache_layer_waits(patch::HiCacheSourceDagIndex(other_lane)).issues.contains("missing_cpu_boundaries"),
            "same timestamps on another thread do not provide call ownership");
}
