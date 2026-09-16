/** @file Small trace timing checks, enabled only in explicit validation builds. */
#include "markov/trace_graph/core/dag_builder.hpp"
#include "markov/trace_graph/core/dag_mutation.hpp"
#include "markov/trace_graph/core/cpu_gap_observation.hpp"
#include "markov/trace_graph/core/client_requests.hpp"
#include "markov/trace_graph/io/trace_manifest_input.hpp"
#include <nlohmann/json.hpp>
#include "markov/trace_graph/simulation/topological_simulator.hpp"
#include "../src/io/trace_channel_join.hpp"
#include "../src/core/dag_builder_stages.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <tuple>

using namespace markov::trace_graph;

void check_native_stream_waits();

namespace {
core::TraceEvent event(std::string name, std::string pid, std::string tid, uint64_t ts, uint64_t dur,
                       std::string category = "cpu_op") {
    core::TraceEvent result;
    result.source_channel = core::TraceSourceChannel::Torch;
    result.name = std::move(name); result.pid = std::move(pid); result.tid = std::move(tid);
    result.ts = ts; result.dur = dur; result.cat = std::move(category);
    return result;
}

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void native_wrapper_setup_cannot_shift_later_call_identities() {
    auto metadata = event("process_name", "900", "0", 0, 0, "");
    metadata.ph = 'M'; metadata.set_arg("name", "CANN");
    const std::string name = "AscendCL@aclrtStreamWaitEvent";
    std::vector<core::TraceEvent> profiler{metadata,
        event(name, "900", "22", 950, 11), event(name, "900", "22", 1016, 4), event(name, "900", "22", 1222, 10)};
    std::vector<core::TraceEvent> wrappers{
        event(name, "20", "22", 0, 975), event(name, "20", "22", 1022, 12), event(name, "20", "22", 1228, 18)};
    for (size_t i = 0; i < wrappers.size(); ++i) {
        wrappers[i].set_arg("Function-Args.stream", "stream" + std::to_string(i));
        wrappers[i].set_arg("Function-Args.event", "event" + std::to_string(i));
    }
    io::detail::join_custom_trace(profiler, wrappers, {});
    for (size_t i = 0; i < wrappers.size(); ++i)
        require(profiler[i + 1].arg("Raw Stream") == "stream" + std::to_string(i)
                && profiler[i + 1].arg("Event Id") == "event" + std::to_string(i),
                "long wrapper setup must not steal the next call's native arguments");
    std::vector<core::TraceEvent> ambiguous{metadata, event(name, "900", "22", 5, 20)};
    wrappers.resize(2);
    wrappers[0].ts = 0; wrappers[0].dur = 10;
    wrappers[1].ts = 20; wrappers[1].dur = 10;
    io::detail::join_custom_trace(ambiguous, wrappers, {});
    require(ambiguous[1].arg("Raw Stream").empty(), "two equally overlapping wrappers cannot prove native identity");
}

void collective_boundaries_only_partition_control_self_time() {
    std::vector<core::TraceEvent> events{event("before", "1", "1", 0, 5),
        event("hicache.control.prefetch_progress", "1", "1", 10, 80),
        event("c10d::allreduce_", "1", "1", 40, 10), event("after", "1", "1", 100, 1)};
    auto original = core::DagBuilder(1).build(events, 0);
    auto collective = event("runtime.cpu_collective", "1", "1", 30, 40, "runtime_diagnostic");
    collective.source_channel = core::TraceSourceChannel::PythonProbe;
    events.push_back(collective);
    auto other_thread = collective; other_thread.tid = "2"; other_thread.ts = 15;
    events.push_back(other_thread);
    auto inside_leaf = collective; inside_leaf.ts = 42; inside_leaf.dur = 6;
    events.push_back(inside_leaf);
    auto graph = core::DagBuilder(1).build(events, 0);
    std::vector<std::pair<uint64_t, uint64_t>> slices;
    for (const auto & observed : graph.events()) {
        if (observed.arg("hicache_control_semantics") == "parent_self_time")
            slices.emplace_back(observed.ts, observed.dur);
        if (observed.name == "c10d::allreduce_")
            require(observed.ts == 40 && observed.dur == 10, "diagnostics must not cut an executable CPU call");
    }
    require(slices == std::vector<std::pair<uint64_t, uint64_t>>{{10, 20}, {30, 10}, {50, 20}, {70, 20}},
            "only same-thread collective boundaries split synthetic self-time without losing work");
    require(graph.runtime_observations().size() == 3, "observations remain metadata, not extra execution");
    const auto baseline = simulation::run_topological_simulation(original);
    const auto split = simulation::run_topological_simulation(graph);
    require(baseline.e2e_us == split.e2e_us, "splitting self-time must preserve full replay");
}

void cann_display_process_is_not_a_second_cpu_thread() {
    auto metadata = event("process_name", "900", "0", 0, 0, "");
    metadata.ph = 'M'; metadata.set_arg("name", "CANN");
    auto scalar = event("aten::_local_scalar_dense", "20", "21", 100, 500);
    auto sync = event("AscendCL@aclrtSynchronizeStreamWithTimeout", "900", "21", 110, 480, "");
    auto kernel = event("kernel", "900", "21", 120, 400, "Kernel");
    kernel.set_arg("Physic Stream Id", "3");
    std::vector<core::TraceEvent> events{metadata, scalar, sync, kernel};
    io::detail::retain_duration_events(events);
    require(events[1].pid == "20", "CANN CPU event must share the framework thread's process");
    require(events[1].arg("profiler_display_pid") == "900", "original display identity remains available");
    require(events[2].pid == "900", "device event identity must not be rewritten");
    auto graph = core::DagBuilder(1).build(std::move(events), 0);
    require(std::ranges::none_of(graph.events(), [](const auto& e) {return e.name == "aten::_local_scalar_dense";}),
            "outer blocking call must not survive as another full-duration CPU leaf");
    require(std::ranges::any_of(graph.events(), [](const auto& e) {return e.name.starts_with("AscendCL@aclrtSynchronize");}),
            "nested synchronization must remain a graph event");

    std::vector<core::TraceEvent> no_metadata{scalar, sync};
    io::detail::retain_duration_events(no_metadata);
    require(no_metadata[1].pid == "900", "missing CANN metadata cannot justify merging processes");
    std::vector<core::TraceEvent> ambiguous{scalar, sync, event("other", "30", "21", 0, 10), metadata};
    io::detail::retain_duration_events(ambiguous);
    require(ambiguous[1].pid == "900", "ambiguous thread ownership cannot merge processes");
    std::vector<core::TraceEvent> unknown{metadata, event("runtime", "900", "unknown", 0, 10, "")};
    io::detail::retain_duration_events(unknown);
    require(unknown[0].pid == "900", "unknown runtime threads must retain their identity");
}

void worker_runtime_keeps_submission_and_device_dependencies() {
    auto metadata = event("process_name", "900", "0", 0, 0, "");
    metadata.ph = 'M'; metadata.set_arg("name", "CANN");
    auto previous = event("previous task", "20", "22", 0, 10, "dequeue");
    auto submit = event("task submission", "20", "21", 0, 100, "enqueue");
    auto wrapper = event("task consumption", "20", "22", 106, 20, "dequeue");
    submit.set_arg("correlation_id", "task"); wrapper.set_arg("correlation_id", "task");
    auto launch = event("Node@launch", "900", "22", 108, 5, "");
    auto kernel = event("kernel", "901", "3", 113, 20, "Kernel");
    launch.set_arg("connection_id", "device"); kernel.set_arg("connection_id", "device");
    kernel.set_arg("Physic Stream Id", "3");
    std::vector<core::TraceEvent> events{metadata, previous, submit, wrapper, launch, kernel};
    io::detail::retain_duration_events(events);
    require(events[3].pid == "20", "dequeue-only threads must identify their nested runtime calls");
    auto graph = core::DagBuilder(1).build(std::move(events), 0);
    require(std::ranges::none_of(graph.events(), [](const auto& e) { return e.name == "task consumption"; }),
            "the dequeue wrapper must not replace its nested device submission");
    const auto node_id = [&](const std::string& name) {
        const auto found = std::ranges::find_if(graph.nodes(), [&](const auto& n) { return graph.event_for_node(n.id).name == name; });
        require(found != graph.nodes().end(), "retained task and device nodes must exist");
        return found->id;
    };
    const auto launch_id = node_id(launch.name);
    require(graph.event_for_node(launch_id).arg("correlation_id") == "task", "runtime leaf inherits its task identity");
    require(graph.node(launch_id).cpu_ready_delay_before == 8, "first runtime leaf follows task readiness");
    for (const auto arrival : {50, 100, 150}) {
        graph.mutable_node(node_id(submit.name)).duration = arrival;
        const auto result = simulation::run_topological_simulation(graph);
        require(result.processed_nodes == graph.node_count(), "runtime dependency graph must remain acyclic");
        require(graph.node(node_id(kernel.name)).completion_time == static_cast<uint64_t>(arrival + 33),
                "device completion must follow changed task submission through the runtime leaf");
    }
}

void device_clock_overlap_does_not_reverse_submission() {
    for (const auto key : {"connection_id", "correlation_id"}) {
        auto launch = event("Node@launch", "20", "21", 105, 5);
        auto kernel = event("kernel", "900", "7", 100, 20, "Kernel");
        launch.set_arg(key, "operation"); kernel.set_arg(key, "operation");
        kernel.set_arg("Physic Stream Id", "7");
        auto graph = core::DagBuilder(1).build({kernel, launch}, 0);
        const auto cpu_node = std::ranges::find_if(graph.nodes(), [](const auto& node) { return node.is_cpu; });
        const auto device_node = std::ranges::find_if(graph.nodes(), [](const auto& node) { return !node.is_cpu; });
        require(cpu_node != graph.nodes().end() && device_node != graph.nodes().end(), "explicit physical stream arguments identify a device node");
        const auto cpu = cpu_node->id, device = device_node->id;
        require(std::ranges::any_of(graph.edges(), [&](const auto& edge) { return edge.active && edge.src == cpu && edge.dst == device; }),
                "submission role, not cross-clock timestamp order, determines causality");
        require(std::ranges::none_of(graph.edges(), [&](const auto& edge) { return edge.active && edge.src == device && edge.dst == cpu; }),
                "device execution cannot become the producer of its own host submission");
        require(graph.node(device).submit_ts == 105, "overlapping device observation retains actual host submission metadata");
        require(simulation::run_topological_simulation(graph).processed_nodes == 2, "role-ordered correlation is acyclic");
    }
}

void event_binding_selects_cpu_role_not_first_timestamp() {
    auto record = event("EVENT_RECORD", "900", "7", 100, 1, "Kernel");
    record.set_arg("Physic Stream Id", "7"); record.set_arg("connection_id", "record");
    auto host_record = event("AscendCL@aclrtRecordEvent", "20", "21", 110, 5);
    host_record.set_arg("connection_id", "record"); host_record.set_arg("Event Id", "event"); host_record.set_arg("Raw Stream", "raw7");
    auto wait = event("EVENT_WAIT", "900", "8", 125, 1, "Kernel");
    wait.set_arg("Physic Stream Id", "8"); wait.set_arg("connection_id", "wait");
    auto host_wait = event("AscendCL@aclrtStreamWaitEvent", "20", "22", 130, 5);
    host_wait.set_arg("connection_id", "wait"); host_wait.set_arg("Event Id", "event");
    core::DagGraph graph({record, host_record, wait, host_wait}, 0);
    auto index = core::create_node_index(graph);
    core::add_event_wait_edges(graph, index);
    require(std::ranges::any_of(graph.edges(), [](const auto& edge) { return edge.src == 0 && edge.dst == 2 && edge.kind == core::DagEdgeKind::Sync; }),
            "event identity comes from the unique CPU view even when device is first");
    require(index.raw_stream_to_lane.at("raw7") == graph.node(0).lane_id, "stream alias comes from that same CPU record");
    auto extra_host = host_record; extra_host.tid = "23";
    core::DagGraph ambiguous({record, host_record, wait, host_wait, extra_host}, 0);
    auto ambiguous_index = core::create_node_index(ambiguous);
    core::add_event_wait_edges(ambiguous, ambiguous_index);
    require(ambiguous.active_edge_count() == 0 && ambiguous_index.raw_stream_to_lane.empty(), "ambiguous CPU views must not guess an event binding");
}

void runtime_diagnostics_do_not_add_or_remove_work() {
    auto before = event("before prepare", "1", "1", 100, 10);
    auto after = event("after prepare", "1", "1", 200, 10);
    auto observation = event("runtime.triton.prepare", "1", "1", 50, 200, "runtime_diagnostic");
    observation.source_channel = core::TraceSourceChannel::PythonProbe;
    auto response = event("runtime.response.scheduler_send", "1", "1", 150, 10, "runtime_diagnostic");
    response.source_channel = core::TraceSourceChannel::PythonProbe;
    auto collective = observation;
    collective.name = "runtime.cpu_collective";
    collective.set_arg("sequence_before", "7");
    auto layer_waits = observation;
    layer_waits.name = "runtime.hicache.layer_waits";
    layer_waits.set_arg("consumer_index", "0");
    auto graph = core::DagBuilder(1).build({before, observation, collective, layer_waits, response, after}, 0);
    require(graph.node_count() == 4 && graph.active_edge_count() == 3, "response boundaries partition the gap without materializing diagnostic work");
    require(graph.hicache_fact_events().empty(), "runtime diagnostic is not a HiCache fact");
    require(graph.runtime_observations().size() == 3 && graph.runtime_observations().front().dur == 200
                && graph.runtime_observations()[1].arg("sequence_before") == "7"
                && graph.runtime_observations().back().arg("consumer_index") == "0",
            "preparation, collective and layer-wait envelopes retain metadata without becoming execution");
    require(simulation::run_topological_simulation(graph).e2e_us == 110, "retain the full 90 us CPU gap");
    graph.set_scope_node_owned(0);
    graph.set_scope_node_owned(1);
    require(simulation::run_gap_excluded_topological_simulation(graph).e2e_us == 20, "response boundaries do not turn gap into execution cost");
}

void event_wait_follows_record_at_host_submission() {
    auto record = event("EVENT_RECORD", "900", "7", 1000, 0, "Kernel");
    record.ts_submicro_ns = 900;
    record.set_arg("Physic Stream Id", "7"); record.set_arg("connection_id", "first");
    auto host_record = event("AscendCL@aclrtRecordEvent", "20", "21", 10, 1);
    host_record.set_arg("connection_id", "first"); host_record.set_arg("Event Id", "handle");
    auto later_record = record; later_record.ts = 900; later_record.tid = "9";
    later_record.set_arg("Physic Stream Id", "9"); later_record.set_arg("connection_id", "later");
    auto later_host = host_record; later_host.ts = 30; later_host.set_arg("connection_id", "later");
    auto wait = event("EVENT_WAIT", "900", "8", 100, 900, "Kernel");
    wait.ts_submicro_ns = 100; wait.dur_submicro_ns = 900;
    wait.set_arg("Physic Stream Id", "8"); wait.set_arg("connection_id", "wait");
    auto host_wait = event("AscendCL@aclrtStreamWaitEvent", "20", "21", 20, 1);
    host_wait.set_arg("connection_id", "wait"); host_wait.set_arg("Event Id", "handle");
    core::DagGraph graph({record, host_record, later_record, later_host, wait, host_wait}, 0);
    auto index = core::create_node_index(graph);
    core::add_event_wait_edges(graph, index);
    require(std::ranges::any_of(graph.edges(), [](const auto& edge) { return edge.src == 0 && edge.dst == 4 && edge.kind == core::DagEdgeKind::Sync; }),
            "wait captures the record submitted before it, even at a fractional device completion boundary");
    require(std::ranges::none_of(graph.edges(), [](const auto& edge) { return edge.src == 2 && edge.dst == 4; }),
            "a later host record cannot replace the event already captured by a wait");
    require(graph.event_for_node(4).ts == wait.ts && graph.event_for_node(4).dur == wait.dur,
            "binding must not shift observed wait timestamps to resolve ordering");
    core::finalize_sync_nodes(graph, index);
    require(graph.node(4).duration == 10, "a proven wait retains the existing bound-sync cost rule");
    core::DagGraph unresolved({wait, host_wait}, 0);
    auto missing = core::create_node_index(unresolved);
    core::add_event_wait_edges(unresolved, missing);
    core::finalize_sync_nodes(unresolved, missing);
    require(unresolved.node(0).duration == wait.dur, "without a wait dependency the observed blocking cost must not disappear");

    auto overlapping_host = host_record; overlapping_host.dur = 15;
    core::DagGraph overlapping({record, overlapping_host, wait, host_wait}, 0);
    auto ambiguous = core::create_node_index(overlapping);
    core::add_event_wait_edges(overlapping, ambiguous);
    core::finalize_sync_nodes(overlapping, ambiguous);
    require(overlapping.active_edge_count() == 0 && overlapping.node(2).duration == wait.dur,
            "overlapping host calls do not establish which event record was captured");

    auto concurrent_record = host_record; concurrent_record.dur = 25;
    auto later_wait = host_wait; later_wait.ts = 40;
    core::DagGraph concurrent({record, concurrent_record, later_record, later_host, wait, later_wait}, 0);
    auto concurrent_index = core::create_node_index(concurrent);
    core::add_event_wait_edges(concurrent, concurrent_index);
    require(concurrent.active_edge_count() == 0, "overlapping record calls do not prove their capture order even after both return");

    auto same_stream_wait = wait; same_stream_wait.tid = "7"; same_stream_wait.ts = 1001;
    same_stream_wait.set_arg("Physic Stream Id", "7");
    core::DagGraph same_stream({record, host_record, same_stream_wait, host_wait}, 0);
    auto same_index = core::create_node_index(same_stream);
    core::add_event_wait_edges(same_stream, same_index);
    require(std::ranges::any_of(same_stream.edges(), [](const auto& edge) { return edge.src == 0 && edge.dst == 2; }),
            "same-stream capture must not fall back to an older record from another stream");

    auto host_sync = host_wait; host_sync.name = "AscendCL@aclrtSynchronizeEvent";
    core::DagGraph cpu_sync({record, host_record, later_record, later_host, host_sync}, 0);
    auto sync_index = core::create_node_index(cpu_sync);
    core::add_event_wait_edges(cpu_sync, sync_index);
    core::add_event_sync_edges(cpu_sync, sync_index);
    require(std::ranges::any_of(cpu_sync.edges(), [](const auto& edge) { return edge.src == 0 && edge.dst == 4; }),
            "host event synchronization also waits for the record captured at its own call");

    same_stream_wait.ts = 1000; same_stream_wait.ts_submicro_ns = 950; same_stream_wait.dur = 10;
    auto fractional = core::DagBuilder(1).build({same_stream_wait, record, host_record, host_wait}, 0);
    require(simulation::run_topological_simulation(fractional).processed_nodes == fractional.node_count(),
            "same-microsecond device order must retain the measured fractional timestamps");

    auto child = event("short CPU child", "20", "21", 100, 0); child.ts_submicro_ns = 800;
    auto fragment = event("CPU self-time fragment", "20", "21", 100, 599);
    core::DagGraph coarse_cpu({child, fragment}, 0);
    auto coarse_index = core::create_node_index(coarse_cpu);
    core::add_sequential_edges(coarse_cpu, coarse_index);
    require(coarse_cpu.edges().size() == 1 && coarse_cpu.edges().front().src == 0 && coarse_cpu.edges().front().dst == 1,
            "integer-partitioned CPU fragments retain their established child/self ordering");
}

void observed_cpu_gap_split_preserves_consumers() {
    const auto source = event("source", "1", "1", 100, 10);
    const auto target = event("target", "1", "1", 200, 10);
    auto observation = event("send", "1", "1", 140, 20);
    auto graph = core::DagBuilder(1).build({source, target}, 0);
    const auto consumer = graph.add_synthetic_node({.name = "other consumer", .duration = 7});
    graph.add_edge(0, consumer, core::DagEdgeKind::Correlation);
    const auto split = core::insert_cpu_gap_observation(graph, observation);
    require(split.has_value(), "an interval wholly inside a unique CPU gap can be connected");
    require(graph.node(0).cpu_gap_after == 30 && graph.node(split->begin).cpu_gap_after == 20
                && graph.node(split->end).cpu_gap_after == 40,
            "three gaps partition the original observed interval");
    require(simulation::run_topological_simulation(graph).e2e_us == 110
                && graph.node(consumer).completion_time == 17 && graph.node(1).completion_time == 110,
            "all original sequential and non-sequential consumers retain their completion times");
    observation.ts = 170;
    require(core::insert_cpu_gap_observation(graph, observation).has_value(), "later disjoint observations share the remaining gap");
    require(simulation::run_topological_simulation(graph).e2e_us == 110, "multiple observations do not duplicate time");
    graph.set_control_exclusion_intervals({{.gpu_id = 0, .start_us = 170, .end_us = 190,
                                            .kind = core::DagControlExclusionKind::PrefillDecode}});
    require(simulation::run_control_topological_simulation(graph).e2e_us == 90,
            "control exclusions use each split interval, not a fraction of the former whole gap");

    const auto rejected = [&](core::DagGraph candidate, const core::TraceEvent & interval) {
        const auto nodes = candidate.node_count(), edges = candidate.edge_count();
        require(!core::insert_cpu_gap_observation(candidate, interval), "unsupported placement must not guess an owner");
        require(candidate.node_count() == nodes && candidate.edge_count() == edges, "rejected placement does not partially edit the graph");
    };
    observation.ts = 155; observation.dur = 20;
    rejected(graph, observation);
    observation.ts = 145; observation.dur = 5; rejected(graph, observation);
    observation.ts = 140; observation.dur = 20; rejected(graph, observation);
    auto raw = core::DagBuilder(1).build({source, target}, 0);
    observation.ts = 105; rejected(raw, observation);
    observation.ts = 190; rejected(raw, observation);
    observation.ts = 140; observation.tid = "unknown"; rejected(raw, observation);
    observation.tid = "1";
    auto changed = raw; changed.mutable_node(0).cpu_gap_after = 80; rejected(changed, observation);
    auto owned = raw; owned.add_scope_gap_duration(0, 10); rejected(owned, observation);
    auto branched = raw;
    const auto branch = branched.add_synthetic_node({.name = "second sequential consumer"});
    branched.add_edge(0, branch, core::DagEdgeKind::Sequential); rejected(branched, observation);
    observation.ts = 110; observation.dur = 90;
    require(core::insert_cpu_gap_observation(raw, observation).has_value(), "intervals may exactly meet both CPU boundaries");
    require(simulation::run_topological_simulation(raw).e2e_us == 110, "zero length surrounding gaps conserve time");
}

void instantaneous_cpu_observation_preserves_time() {
    auto graph = core::DagBuilder(1).build({event("before", "1", "1", 100, 10), event("after", "1", "1", 200, 10)}, 0);
    const auto before = simulation::run_topological_simulation(graph).e2e_us;
    auto point = event("runtime.request.dispatch_ready", "1", "1", 150, 0, "runtime_diagnostic");
    point.source_channel = core::TraceSourceChannel::PythonProbe;
    const auto boundary = core::insert_cpu_gap_observation(graph, point);
    require(boundary.has_value(), "a measured instant inside a unique CPU gap must be connectable");
    require(boundary->begin == boundary->end && graph.node_count() == 3, "an instant is one point, not a synthetic interval");
    require(graph.node(boundary->begin).duration == 0 && !graph.node(boundary->begin).counts_toward_e2e,
            "observing a point neither adds cost nor selects the business endpoint");
    require(simulation::run_topological_simulation(graph).e2e_us == before, "point insertion preserves full replay");
    require(graph.node(0).cpu_gap_after == 40 && graph.node(boundary->begin).cpu_gap_after == 50, "the old gap is only partitioned");
    require(!core::insert_cpu_gap_observation(graph, point), "ambiguous repeated boundaries do not guess an owner");
    for (size_t id = 0; id < 2; ++id) graph.set_scope_node_owned(id);
    require(simulation::run_gap_excluded_topological_simulation(graph).e2e_us == 20, "point insertion does not turn gap into scoped cost");

    auto envelope = point; envelope.name = "runtime.request.receive"; envelope.ts = 140; envelope.dur = 10;
    auto with_receive = core::DagBuilder(1).build({event("before", "1", "1", 100, 10), event("after", "1", "1", 200, 10), envelope, point}, 0);
    require(with_receive.node_count() == 4, "receive envelope adds a start point, not a second execution interval");
    require(simulation::run_topological_simulation(with_receive).e2e_us == before, "receive start and dispatch points preserve the original timeline");
}

void cache_facts_do_not_create_device_barriers() {
    auto first = event("first host work", "1", "1", 0, 5);
    auto submit = event("next submission", "1", "1", 110, 1, "enqueue");
    submit.set_arg("correlation_id", "next");
    auto query = event("unrelated background query", "1", "2", 105, 1);
    auto background = event("background kernel", "1", "10", 10, 50, "Kernel");
    background.set_arg("Physic Stream Id", "10");
    auto compute = event("request kernel", "1", "11", 120, 10, "Kernel");
    compute.set_arg("Physic Stream Id", "11");
    compute.set_arg("correlation_id", "next");
    const auto fact = [](const char* role, const char* request, uint64_t ts) {
        auto value = event("cache fact", "1", "1", ts, 0);
        value.source_channel = core::TraceSourceChannel::PythonProbe;
        value.set_arg("fact", "{}");
        value.set_arg("fact.class", "workload_identity");
        value.set_arg("fact.role", role);
        value.set_arg("request_id", request);
        value.set_arg("lifecycle_kind", "finished");
        return value;
    };
    auto graph = core::DagBuilder(1).build({first, submit, query, background, compute,
        fact("cache_lookup_input", "one", 5), fact("cache_lifecycle_commit", "one", 80),
        fact("cache_lookup_input", "two", 100)}, 0);
    require(graph.hicache_fact_events().size() == 3 && graph.node_count() == 5, "cache facts remain metadata, not execution nodes");
    const auto find = [&](const char* name) {
        return std::ranges::find_if(graph.nodes(), [&](const auto& node) { return graph.event_for_node(node.id).name == name; })->id;
    };
    const auto request = find("request kernel"), unrelated = find("background kernel");
    (void)simulation::run_topological_simulation(graph);
    const auto before = graph.node(request).completion_time;
    graph.set_node_duration(unrelated, 1000);
    (void)simulation::run_topological_simulation(graph);
    require(graph.node(request).completion_time == before,
            "cache lookup facts cannot turn unrelated streams and background threads into a request barrier");
    graph.set_node_duration(find("next submission"), 11);
    (void)simulation::run_topological_simulation(graph);
    require(graph.node(request).completion_time == before + 10, "real submission dependencies remain active");
}

void serial_http_clients_follow_responses_not_background_work() {
    core::DagGraph graph;
    const auto boundary = [&](const char* stage, const char* role, const char* id, uint64_t ts) {
        auto node = graph.add_synthetic_node({.name = stage, .category = "observed_boundary",
            .attrs = {{"observed_interval", stage}, {"interval_boundary", role}, {"request_ids", std::string("[\"") + id + "\"]"}}});
        graph.mutable_event_for_node(node).ts = ts;
        return node;
    };
    const auto receive1 = boundary("runtime.request.socket_received", "point", "one", 10);
    const auto compute1 = graph.add_synthetic_node({.name = "first compute", .duration = 100, .counts_toward_e2e = true});
    const auto send1 = boundary("runtime.response.scheduler_send", "end", "one", 110);
    const auto receive2 = boundary("runtime.request.socket_received", "point", "two", 130);
    const auto compute2 = graph.add_synthetic_node({.name = "second compute", .duration = 50, .counts_toward_e2e = true});
    const auto send2 = boundary("runtime.response.scheduler_send", "end", "two", 180);
    const auto background = graph.add_synthetic_node({.name = "background work", .duration = 500, .counts_toward_e2e = true});
    graph.add_edge(receive1, compute1, core::DagEdgeKind::Sync);
    graph.add_edge(compute1, send1, core::DagEdgeKind::Sync);
    graph.add_edge(send1, receive2, core::DagEdgeKind::Sequential);
    graph.mutable_node(send1).cpu_gap_after = 20;
    graph.add_edge(receive2, compute2, core::DagEdgeKind::Sync);
    graph.add_edge(compute2, send2, core::DagEdgeKind::Sync);
    const std::vector<core::ClientRequestTiming> requests{{"one", 0, 115, 10}, {"two", 120, 185, 130}};
    auto incomplete = graph;
    auto invalid = requests; invalid.back().request_id = "missing";
    require(core::connect_client_requests(incomplete, invalid).status != "connected" && incomplete.node_count() == graph.node_count(),
            "missing source observations leave the graph unchanged");
    const auto chain = core::connect_client_requests(graph, requests);
    require(chain.status == "connected" && chain.requests.size() == 2, "complete source requests form a client chain");
    for (const auto [compute, expected] : {std::pair{100, 185}, {50, 135}, {150, 235}}) {
        graph.set_node_duration(compute1, compute);
        require(simulation::run_topological_simulation(graph).e2e_us == 500, "background work remains in the graph metric");
        require(graph.node(chain.requests.back().completion).completion_time == static_cast<uint64_t>(expected),
                "HTTP completion follows changed compute without double-counting the existing server gap");
    }
    graph.set_node_duration(background, 1000);
    graph.mutable_node(send1).cpu_gap_after = 60;
    (void)simulation::run_topological_simulation(graph);
    require(graph.node(chain.requests.back().completion).completion_time == 275, "server availability can dominate client availability");
}

void client_input_reads_only_declared_source_observations() {
    using Json = nlohmann::json;
    char pattern[] = "/tmp/markov-client-XXXXXX";
    const auto * directory = mkdtemp(pattern);
    require(directory != nullptr, "create isolated client input fixture");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{directory};
    const auto manifest_path = (cleanup.path / "manifest.json").string();
    const auto report_path = (cleanup.path / "report.json").string();
    const auto probe_path = (cleanup.path / "probe.json").string();
    const auto row = [](const char* id, double start, double end) {
        return Json{{"kind", "request"}, {"status", "ok"}, {"logical_request_id", id}, {"start_time_ms", start}, {"end_time_ms", end}};
    };
    Json report = {{"status", "completed"}, {"formal_window", {{"formal_begin_ms", 0}, {"formal_end_ms", 0.185}}},
                   {"requests", Json::array({row("one", 0, 0.115), row("two", 0.120, 0.185)})}};
    Json events = Json::array();
    for (const auto& [id, start] : {std::pair{"one", 5}, {"two", 125}})
        events.push_back({{"name", "runtime.request.tokenizer_submit"}, {"ph", "X"}, {"ts", start}, {"dur", 5},
                          {"pid", 1}, {"tid", 1}, {"args", {{"request_ids", Json::array({id})}}}});
    Json manifest;
    manifest["bench"]["workload_report_files"] = Json::array({{{"path", report_path}, {"exists", true}}});
    manifest["sidecar"]["python_probe_files"] = Json::array({{{"path", probe_path}, {"exists", true}}});
    std::ofstream(report_path) << report;
    std::ofstream(probe_path) << events;
    std::ofstream(manifest_path) << manifest;
    const auto input = io::load_client_requests_from_manifest(manifest_path);
    require(input.status == "ready" && input.requests.size() == 2 && input.requests.back().frontend_end_us == 130,
            "source report and submission observations are joined by request identity");
    const auto client_events = events;
    events = Json::array();
    for (const auto ts : {10, 100, 300}) events.push_back({{"name", "runtime.triton.prepare"}, {"cat", "runtime_diagnostic"},
        {"ph", "X"}, {"ts", ts}, {"dur", 5}, {"pid", 1}, {"tid", 1}});
    for (const auto ts : {10, 100, 300}) events.push_back({{"name", "CPU work"}, {"cat", "cpu_op"},
        {"ph", "X"}, {"ts", ts}, {"dur", 5}, {"pid", 1}, {"tid", 1}});
    std::ofstream(probe_path) << events;
    io::ManifestTraceInputOptions window;
    window.window_start_us = 90; window.window_end_us = 200;
    auto traces = io::load_trace_inputs_from_manifest(manifest_path, window);
    auto graph = core::DagBuilder(1).build(std::move(traces.front().events), 0);
    require(graph.runtime_observations().size() == 2 && graph.node_count() == 1,
            "retain prelude and formal preparation metadata, but neither prelude work nor post-window preparation");
    require(graph.event_for_node(0).ts == 100, "preparation context cannot move the execution window origin");
    std::ofstream(probe_path) << client_events;
    const Json barrier = {{"kind", "barrier"}, {"status", "ok"}, {"start_time_ms", 0.117}, {"end_time_ms", 0.119}};
    report["requests"].insert(report["requests"].begin() + 1, barrier);
    std::ofstream(report_path) << report;
    require(io::load_client_requests_from_manifest(manifest_path).status == "formal_control_step_not_modeled",
            "a control step inside the HTTP window is not silently converted into client think time");
    report["requests"][1].erase("start_time_ms");
    std::ofstream(report_path) << report;
    require(io::load_client_requests_from_manifest(manifest_path).status == "formal_step_without_timing", "untimed steps cannot disappear");
    std::ofstream(manifest_path) << Json::object();
    require(io::load_client_requests_from_manifest(manifest_path).requests.empty(), "old manifests do not invent client observations");
}

void queue_wait_follows_task_arrival() {
    auto worker = event("previous task", "1", "2", 0, 10);
    auto submit = event("task submission", "1", "1", 0, 100, "enqueue");
    auto next = event("next task", "1", "2", 106, 5, "dequeue");
    submit.set_arg("correlation_id", "1");
    next.set_arg("correlation_id", "1");
    auto graph = core::DagBuilder(1).build({worker, submit, next}, 0);
    auto node_id = [](const core::DagGraph& g, const std::string& name) {
        return std::ranges::find_if(g.nodes(), [&](const auto& n) { return g.event_for_node(n.id).name == name; })->id;
    };
    const auto worker_id = node_id(graph, worker.name);
    const auto submit_id = node_id(graph, submit.name);
    const auto next_id = node_id(graph, next.name);
    require(graph.node_count() == 3 && graph.edge_count() == 2, "queue normalization must retain the original topology");
    require(graph.node(next_id).cpu_ready_delay_before == 6, "retain measured time after task and worker become ready");
    require(graph.node(worker_id).cpu_gap_after == 0 && graph.node(worker_id).original_cpu_gap_after == 96,
            "task arrival replaces idle wait without rewriting the observation");
    for (const auto [arrival, busy, expected] : {std::tuple{100, 10, 111}, {50, 10, 61}, {150, 10, 161}, {50, 80, 91}}) {
        graph.mutable_node(submit_id).duration = arrival;
        graph.mutable_node(worker_id).duration = busy;
        require(simulation::run_topological_simulation(graph).e2e_us == static_cast<uint64_t>(expected),
                "CPU task must follow whichever prerequisite finishes last, plus the measured remainder");
    }
    graph.mutable_node(submit_id).duration = 100;
    graph.mutable_node(worker_id).duration = 10;
    for (const auto& n : graph.nodes()) graph.set_scope_node_owned(n.id);
    require(simulation::run_gap_excluded_topological_simulation(graph).e2e_us == 105,
            "legacy component scope excludes the residual queue remainder");
    graph.set_control_exclusion_intervals({{0, 100, 106, core::DagControlExclusionKind::PrefillDecode}});
    require(simulation::run_control_topological_simulation(graph).e2e_us == 105,
            "control replay removes only the measured remainder overlapping its exclusion window");

    worker.dur = 80; submit.dur = 50; next.ts = 86;
    auto queued = core::DagBuilder(1).build({worker, submit, next}, 0);
    queued.mutable_node(node_id(queued, submit.name)).duration = 100;
    require(simulation::run_topological_simulation(queued).e2e_us == 111,
            "a source-busy worker must still preserve the remainder if target submission becomes slower");
    worker.dur = 10; submit.dur = 100; next.ts = 40;
    auto overlap = core::DagBuilder(1).build({worker, submit, next}, 0);
    require(overlap.node(node_id(overlap, next.name)).cpu_ready_delay_before == 0,
            "overlapping observations do not establish a finish-to-start queue delay");
    next.ts = 106; worker.dur = 10; next.set_arg("correlation_id", "unmatched");
    auto unknown = core::DagBuilder(1).build({worker, submit, next}, 0);
    require(unknown.node(node_id(unknown, worker.name)).cpu_gap_after == 96,
            "unmatched queue events keep their observed gap");
}

void cpu_queue_order_follows_target_arrivals() {
    auto a = event("submit A", "1", "1", 0, 20, "enqueue"); a.set_arg("correlation_id", "A");
    auto b = event("submit B", "1", "2", 0, 60, "enqueue"); b.set_arg("correlation_id", "B");
    auto first = event("task A first", "1", "3", 25, 4); first.set_arg("correlation_id", "A");
    auto last = event("task A last", "1", "3", 31, 4); last.set_arg("correlation_id", "A");
    auto other = event("task B", "1", "3", 65, 15); other.set_arg("correlation_id", "B");
    auto graph = core::DagBuilder(1).build({a, b, first, last, other}, 0);
    const auto find = [&](const char* name) {
        return std::ranges::find_if(graph.nodes(), [&](const auto& node) { return graph.event_for_node(node.id).name == name; })->id;
    };
    const auto submit_a = find("submit A"), submit_b = find("submit B");
    const auto task_a = find("task A last"), task_b = find("task B");
    // Reuse the graph: each full replay must replace the previous resource order.
    for (const auto [a_cost, b_cost, a_end, b_end] : {
            std::tuple{20, 60, 35, 80}, {20, 10, 45, 30}, {100, 10, 115, 30},
            {20, 20, 35, 55}, {20, 25, 35, 55}, {20, 60, 35, 80}}) {
        graph.set_node_duration(submit_a, a_cost);
        graph.set_node_duration(submit_b, b_cost);
        const auto full = simulation::run_topological_simulation(graph);
        require(full.cpu_queue_count == 1 && full.cpu_task_count == 2, "continuous worker leaves form two tasks sharing one queue");
        require(graph.node(task_a).completion_time == static_cast<uint64_t>(a_end)
                && graph.node(task_b).completion_time == static_cast<uint64_t>(b_end),
                "FIFO follows target arrivals, preserves internal work, and charges readiness after the worker is free");
        require(simulation::run_control_topological_simulation(graph).e2e_us == full.e2e_us,
                "resolved queue order must be represented in the output DAG, not hidden inside simulation");
    }
    a.dur = 30;
    auto overlap = core::DagBuilder(1).build({a, b, first, last, other}, 0);
    const auto conservative = simulation::run_topological_simulation(overlap);
    require(conservative.submission_overlap_count == 1 && conservative.submission_overlap_total_us == 5
            && conservative.submission_overlap_max_us == 5, "return-time approximation must disclose source publication overlap");
    auto blocked = core::DagBuilder(1).build({a, b, first, last, other}, 0);
    const auto locate = [&](const char* name) {
        return std::ranges::find_if(blocked.nodes(), [&](const auto& node) { return blocked.event_for_node(node.id).name == name; })->id;
    };
    blocked.add_edge(locate("task B"), locate("task A first"), core::DagEdgeKind::Sync);
    bool rejected = false;
    try { (void)simulation::run_topological_simulation(blocked); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "a task blocked on a later task in the same FIFO must not return partial timing");
    other.set_arg("correlation_id", "missing");
    auto partial = core::DagBuilder(1).build({a, b, first, last, other}, 0);
    require(simulation::run_topological_simulation(partial).cpu_queue_count == 0, "a partially identified worker is not silently reordered");
}

void layer_wait_clock_is_local_to_each_trace() {
    using Json = nlohmann::json;
    char pattern[] = "/tmp/markov-clock-XXXXXX";
    const auto * directory = mkdtemp(pattern);
    require(directory != nullptr, "create isolated clock fixture");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{directory};
    const auto manifest_path = (cleanup.path / "manifest.json").string();
    Json manifest;
    for (int rank : {1, 2}) {
        const auto trace_path = (cleanup.path / ("trace_pid" + std::to_string(rank) + ".json")).string();
        const auto probe_path = (cleanup.path / ("probe_pid" + std::to_string(rank) + ".json")).string();
        std::ofstream(trace_path) << Json::array({{{"name", "CPU work"}, {"cat", "cpu_op"}, {"ph", "X"},
            {"ts", 100}, {"dur", 1}, {"pid", rank}, {"tid", 1}}});
        std::ofstream(probe_path) << Json::array({{{"name", "runtime.hicache.layer_waits"}, {"cat", "runtime_diagnostic"},
            {"ph", "X"}, {"ts", 100}, {"dur", 1}, {"pid", rank}, {"tid", 1},
            {"args", {{"wait_clock", "npu_syscnt"}, {"wait_intervals", Json::array({{0, 90, 131}})}}}}});
        manifest["trace"]["torch_trace_files"].push_back({{"path", trace_path}, {"host_clock", {
            {"clock", "npu_syscnt"}, {"origin_tick", 100}, {"origin_ns", 1'700'000'000'000'000'123LL + rank}, {"ns_per_tick", rank * 10}}}});
        manifest["sidecar"]["python_probe_files"].push_back({{"path", probe_path}});
    }
    std::ofstream(manifest_path) << manifest;
    io::ManifestTraceInputOptions options;
    options.threads = 2;
    const auto inputs = io::load_trace_inputs_from_manifest(manifest_path, options);
    require(inputs.size() == 2, "two rank-local trace inputs");
    for (size_t i = 0; i < inputs.size(); ++i) {
        const auto & events = inputs[i].events;
        const auto found = std::ranges::find_if(events, [](const auto & e) { return e.name == "runtime.hicache.layer_waits"; });
        require(found != events.end() && found->arg("wait_clock") == "profiler_ns", "counter clock is explicitly normalized");
        const int64_t rank = i + 1;
        const auto row = Json::parse(found->arg("wait_intervals")).at(0);
        require(row[1].get<int64_t>() == 1'700'000'000'000'000'123LL + rank - 100 * rank
            && row[2].get<int64_t>() == 1'700'000'000'000'000'123LL + rank + 310 * rank,
            "rank-local offsets preserve nanoseconds and samples before the clock anchor");
        require(found->ts == 100 && found->dur == 1, "batch wall-clock envelope is not silently shifted");
    }
    manifest["trace"]["torch_trace_files"][0].erase("host_clock");
    std::ofstream(manifest_path) << manifest;
    bool rejected = false;
    try { (void)io::load_trace_inputs_from_manifest(manifest_path, options); }
    catch (const std::runtime_error & error) { rejected = std::string(error.what()).find("host_clock") != std::string::npos; }
    require(rejected, "raw counters cannot be silently treated as epoch timestamps");
    const auto probe_path = manifest["sidecar"]["python_probe_files"][0]["path"].get<std::string>();
    Json old_probe;
    std::ifstream(probe_path) >> old_probe;
    old_probe[0]["args"].erase("wait_clock");
    std::ofstream(probe_path) << old_probe;
    const auto old_input = io::load_trace_inputs_from_manifest(manifest_path, options);
    const auto & events = old_input[0].events;
    const auto old = std::ranges::find_if(events, [](const auto & e) { return e.name == "runtime.hicache.layer_waits"; });
    require(old->arg("wait_clock").empty() && Json::parse(old->arg("wait_intervals"))[0][1] == 90,
            "historical wall-clock observations remain historical, not repaired by an invented offset");
}

void removed_cpu_tasks_do_not_become_residual_waits() {
    std::vector<core::TraceEvent> events;
    for (size_t i = 0; i < 3; ++i) {
        const auto identity = std::to_string(i);
        auto submit = event("submit " + identity, "1", std::to_string(i + 1), 0, i + 1, "enqueue");
        auto task = event("task " + identity, "1", "4", 2 + 11 * i, 10);
        submit.set_arg("correlation_id", identity); task.set_arg("correlation_id", identity);
        events.push_back(submit); events.push_back(task);
    }
    for (const auto [removed, expected] : {std::pair{0, 24}, {1, 23}, {2, 23}}) {
        auto graph = core::DagBuilder(1).build(events, 0);
        const auto find = [&](const std::string& name) {
            return std::ranges::find_if(graph.nodes(), [&](const auto& node) { return graph.event_for_node(node.id).name == name; })->id;
        };
        require(simulation::run_topological_simulation(graph).e2e_us == 34, "source queue includes three tasks and measured ready delays");
        core::DagMutationPlan plan{.component = "task_removal"};
        plan.disable_nodes = {find("submit " + std::to_string(removed)), find("task " + std::to_string(removed))};
        if (removed == 1) plan.add_edges.push_back({.src = core::DagNodeRef::existing(find("task 0")),
            .dst = core::DagNodeRef::existing(find("task 2")), .kind = core::DagEdgeKind::Sequential});
        (void)core::apply_dag_mutation_plan(graph, plan);
        for (int replay = 0; replay < 2; ++replay) {
            const auto result = simulation::run_topological_simulation(graph);
            require(result.cpu_task_count == 2 && result.e2e_us == static_cast<uint64_t>(expected),
                    "removing a task cannot reclassify its service as the next task's residual delay");
        }
        if (removed != 2) {
            require(graph.node(find("task 2")).cpu_ready_delay_before == 1, "source ready delay survives task removal");
            graph.set_node_duration(find("submit 2"), 40);
            require(simulation::run_topological_simulation(graph).e2e_us == 51,
                    "retained task still follows its target arrival plus measured delay and work");
        }
    }
}

void response_endpoint_preserves_background_resource_dependencies() {
    core::DagGraph graph;
    const auto background = graph.add_synthetic_node({.name = "earlier background work", .duration = 40});
    const auto work = graph.add_synthetic_node({.name = "business work", .duration = 10});
    const auto response = graph.add_synthetic_node({.name = "client response", .duration = 5, .counts_toward_e2e = true});
    const auto poll = graph.add_synthetic_node({.name = "later background poll", .duration = 100});
    graph.add_edge(background, work, core::DagEdgeKind::Sequential);
    graph.add_edge(work, response, core::DagEdgeKind::Sequential);
    graph.add_edge(work, poll, core::DagEdgeKind::Sequential);
    const auto replay = simulation::run_topological_simulation(graph);
    require(replay.e2e_us == 55 && replay.processed_nodes == 4 && graph.node(poll).completion_time == 150,
            "response completion retains earlier resource contention but not unrelated later work");
    graph.mutable_node(background).duration = 80;
    require(simulation::run_topological_simulation(graph).e2e_us == 95,
            "a non-endpoint background task still delays its business consumer");
    graph.mutable_node(work).cpu_gap_after = 7;
    require(simulation::run_topological_simulation(graph).e2e_us == 102,
            "selecting a response endpoint must not erase the observed gap on its incoming path");
    graph.add_edge(poll, response, core::DagEdgeKind::Mutation);
    require(simulation::run_topological_simulation(graph).e2e_us == 202,
            "background completion must be included when the response actually depends on it");
}
}

int main() {
    collective_boundaries_only_partition_control_self_time();
    native_wrapper_setup_cannot_shift_later_call_identities();
    cann_display_process_is_not_a_second_cpu_thread();
    worker_runtime_keeps_submission_and_device_dependencies();
    device_clock_overlap_does_not_reverse_submission();
    event_binding_selects_cpu_role_not_first_timestamp();
    event_wait_follows_record_at_host_submission();
    queue_wait_follows_task_arrival();
    runtime_diagnostics_do_not_add_or_remove_work();
    observed_cpu_gap_split_preserves_consumers();
    instantaneous_cpu_observation_preserves_time();
    cache_facts_do_not_create_device_barriers();
    serial_http_clients_follow_responses_not_background_work();
    client_input_reads_only_declared_source_observations();
    cpu_queue_order_follows_target_arrivals();
    removed_cpu_tasks_do_not_become_residual_waits();
    layer_wait_clock_is_local_to_each_trace();
    check_native_stream_waits();
    response_endpoint_preserves_background_resource_dependencies();
    std::cout << "Trace timing checks passed\n";
}
