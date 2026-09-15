/** @file Small trace timing checks, enabled only in explicit validation builds. */
#include "markov/trace_graph/core/dag_builder.hpp"
#include "markov/trace_graph/simulation/topological_simulator.hpp"
#include "../src/io/trace_channel_join.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <tuple>

using namespace markov::trace_graph;

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

void runtime_diagnostics_do_not_add_or_remove_work() {
    auto before = event("before prepare", "1", "1", 100, 10);
    auto after = event("after prepare", "1", "1", 200, 10);
    auto observation = event("runtime.triton.prepare", "1", "1", 50, 200, "runtime_diagnostic");
    observation.source_channel = core::TraceSourceChannel::PythonProbe;
    auto graph = core::DagBuilder(1).build({before, observation, after}, 0);
    require(graph.node_count() == 2 && graph.edge_count() == 1, "diagnostic envelope is not executable work");
    require(graph.hicache_fact_events().empty(), "runtime diagnostic is not a HiCache fact");
    require(simulation::run_topological_simulation(graph).e2e_us == 110, "retain the full 90 us CPU gap");
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
}

int main() {
    cann_display_process_is_not_a_second_cpu_thread();
    worker_runtime_keeps_submission_and_device_dependencies();
    queue_wait_follows_task_arrival();
    runtime_diagnostics_do_not_add_or_remove_work();
    std::cout << "Trace timing checks passed\n";
}
