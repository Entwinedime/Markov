/** Small checks for model-created tasks sharing an observed CPU worker. */
#include "markov/trace_graph/core/dag_builder.hpp"
#include "markov/trace_graph/core/dag_mutation.hpp"
#include "markov/trace_graph/simulation/topological_simulator.hpp"
#include <algorithm>
#include <stdexcept>
#include <tuple>

using namespace markov::trace_graph;
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
core::TraceEvent event(const char* name, const char* tid, uint64_t ts, uint64_t dur,
                       const char* category, const char* correlation) {
    core::TraceEvent e;
    e.source_channel = core::TraceSourceChannel::Torch;
    e.name = name; e.pid = "1"; e.tid = tid; e.ts = ts; e.dur = dur; e.cat = category;
    e.set_arg("correlation_id", correlation);
    return e;
}
} // namespace

namespace {
void dynamic_worker_task(uint64_t arrival, uint64_t first_service, uint64_t expected_new, uint64_t expected_last) {
    auto graph = core::DagBuilder(1).build({
        event("submit A", "1", 0, 20, "enqueue", "A"),
        event("submit B", "2", 0, 60, "enqueue", "B"),
        event("task A", "3", 25, 10, "cpu_op", "A"),
        event("task B", "3", 65, 15, "cpu_op", "B")}, 0);
    const auto find = [&](const char* name) {
        return std::ranges::find_if(graph.nodes(), [&](const auto& n) { return graph.event_for_node(n.id).name == name; })->id;
    };
    const auto a = find("task A"), b = find("task B");
    graph.set_node_duration(a, first_service);
    const auto lane = std::string(graph.node_lane_key(a));
    const auto finished = graph.add_synthetic_node({.name = "already finished", .lane_key = "main"});
    const auto decision = graph.add_synthetic_node({.name = "target decides", .lane_key = "main"});
    graph.add_edge(finished, decision, core::DagEdgeKind::Mutation);
    size_t worker = 0;
    const auto result = simulation::run_topological_simulation(graph, {}, [&](size_t id, uint64_t, simulation::FutureDag& future) {
        if (id != decision) return;
        const auto submit = future.append({.name = "new submission", .lane_key = "producer", .duration = arrival});
        const core::DagSyntheticNodeSpec spec{.name = "new worker", .lane_key = lane, .duration = 7,
            .counts_toward_e2e = true, .cpu_task_ready_delay_us = 3};
        const auto reject = [&](const core::DagSyntheticNodeSpec& invalid, size_t producer, size_t member) {
            const auto count = graph.node_count();
            bool rejected = false;
            try { (void)future.append_cpu_task(invalid, producer, member); }
            catch (const std::invalid_argument&) { rejected = true; }
            require(rejected && graph.node_count() == count, "invalid queue insertion cannot mutate graph or queue state");
        };
        reject(spec, finished, a);
        reject(spec, submit, decision);
        auto wrong_lane = spec;
        wrong_lane.lane_key = "unobserved worker";
        reject(wrong_lane, submit, a);
        worker = future.append_cpu_task(spec, submit, a);
        reject(spec, submit, a);
    });
    require(result.cpu_queue_count == 1 && result.cpu_task_count == 3, "dynamic task must compete on the existing worker");
    require(graph.node(worker).completion_time == expected_new && graph.node(b).completion_time == expected_last,
            "dynamic queue preserves submission FIFO and measured worker readiness");
    const auto saved = graph.nodes();
    (void)simulation::run_topological_simulation(graph);
    for (const auto& n : saved)
        require(n.simulation_start == graph.node(n.id).simulation_start && n.completion_time == graph.node(n.id).completion_time,
                "dynamically registered CPU tasks must replay with identical timing");
}
} // namespace

void check_cpu_task_insertion() {
    dynamic_worker_task(10, 10, 20, 80);
    dynamic_worker_task(25, 10, 45, 80);
    dynamic_worker_task(62, 10, 90, 80);
    dynamic_worker_task(100, 10, 110, 80);
    dynamic_worker_task(25, 100, 135, 155);
    auto graph = core::DagBuilder(1).build({
        event("submit A", "1", 0, 20, "enqueue", "A"),
        event("submit B", "2", 0, 60, "enqueue", "B"),
        event("task A", "3", 25, 10, "cpu_op", "A"),
        event("task B", "3", 65, 15, "cpu_op", "B")}, 0);
    const auto find = [&](const char* name) {
        return std::ranges::find_if(graph.nodes(), [&](const auto& n) { return graph.event_for_node(n.id).name == name; })->id;
    };
    const auto a = find("task A"), b = find("task B");
    const auto lane = std::string(graph.lane_key(graph.node(a).lane_id));
    core::DagMutationPlan plan;
    plan.component = "test";
    plan.synthetic_nodes.push_back({.synthetic_id = "submit", .node = {
        .name = "new submission", .lane_key = "new producer", .duration = 10}});
    plan.synthetic_nodes.push_back({.synthetic_id = "worker", .node = {
        .name = "new worker", .lane_key = lane, .duration = 7, .counts_toward_e2e = true,
        .cpu_task_ready_delay_us = 3}});
    plan.add_edges.push_back({.src = core::DagNodeRef::synthetic("submit"), .dst = core::DagNodeRef::synthetic("worker"),
                              .kind = core::DagEdgeKind::Correlation});
    const auto mutation = core::apply_dag_mutation_plan(graph, plan);
    const auto submit = mutation.synthetic_node_ids.at("submit"), worker = mutation.synthetic_node_ids.at("worker");
    const auto pristine = graph;
    for (const auto [arrival, completion] : {std::pair{10, 20}, {25, 45}, {62, 90}, {100, 110}, {10, 20}}) {
        graph.set_node_duration(submit, arrival);
        const auto full = simulation::run_topological_simulation(graph);
        require(full.cpu_queue_count == 1 && full.cpu_task_count == 3, "new task must join the existing worker FIFO");
        require(graph.node(worker).completion_time == static_cast<uint64_t>(completion) && graph.node(b).completion_time == 80,
                "arrival order, worker service and readiness remainder determine completion");
        require(simulation::run_control_topological_simulation(graph).e2e_us == full.e2e_us,
                "materialized queue edges retain the same result on replay");
        auto timed = pristine;
        timed.set_node_duration(submit, arrival);
        size_t calls = 0;
        uint64_t previous = 0;
        const auto causal = simulation::run_topological_simulation(timed, [&](size_t id, uint64_t at, uint64_t duration) {
            require(at >= previous && at == graph.node(id).simulation_start,
                    "cost callback follows actual CPU queue admission without charging readiness twice");
            previous = at;
            ++calls;
            return duration;
        });
        require(calls == timed.active_node_count() && causal.e2e_us == full.e2e_us,
                "identity cost callbacks preserve queue replay and visit every active node once");
    }
    graph.set_node_duration(a, 100);
    graph.set_node_duration(submit, 25);
    (void)simulation::run_topological_simulation(graph);
    require(graph.node(worker).completion_time == 135 && graph.node(b).completion_time == 155,
            "inserted task waits behind busy source work and delays the later arrival");
    graph = pristine;
    const auto record = graph.add_synthetic_node({.name = "layer readiness", .is_cpu = false, .lane_key = "H2D", .duration = 120});
    const auto wait = graph.add_synthetic_node({.name = "layer wait", .is_cpu = false, .lane_key = "compute"});
    const auto kernel = graph.add_synthetic_node({.name = "consumer", .is_cpu = false, .lane_key = "compute", .duration = 10});
    graph.add_edge(worker, wait, core::DagEdgeKind::Correlation);
    graph.add_edge(record, wait, core::DagEdgeKind::Sync);
    graph.add_edge(wait, kernel, core::DagEdgeKind::Stream);
    for (const auto [ready, completion] : {std::pair{120, 130}, {5, 30}}) {
        graph.set_node_duration(record, ready);
        (void)simulation::run_topological_simulation(graph);
        require(graph.node(kernel).completion_time == static_cast<uint64_t>(completion),
                "device consumer follows both the queued wait submission and its own layer readiness");
    }
    core::DagGraph prefix;
    prefix.add_synthetic_node({.name = "independent", .lane_key = "other", .duration = 1});
    auto merged = core::DagGraph::merge({prefix, pristine});
    require(simulation::run_topological_simulation(merged).cpu_task_count == 3
            && merged.node(worker + 1).completion_time == 20,
            "graph merge retains explicit task identity and remaps its submission edge");
    graph = pristine;
    graph.set_node_duration(submit, 25);
    for (const auto removed : {a, b}) {
        core::DagMutationPlan removal;
        removal.component = "test";
        removal.disable_nodes.push_back(removed);
        (void)core::apply_dag_mutation_plan(graph, removal);
        const auto full = simulation::run_topological_simulation(graph);
        require(full.cpu_queue_count == 1 && graph.node(worker).completion_time == 35,
                "new task survives removal of some or all observed tasks without recharging their costs");
    }
    for (int invalid = 0; invalid < 3; ++invalid) {
        graph = pristine;
        if (invalid == 0) {
            for (size_t id = 0; id < graph.edge_count(); ++id)
                if (graph.edge(id).dst == worker) graph.disable_edge(id);
        }
        if (invalid == 1) graph.add_edge(find("submit A"), worker, core::DagEdgeKind::Correlation);
        if (invalid == 2) graph.mutable_event_for_node(a).set_arg("correlation_id", "unknown");
        bool rejected = false;
        try { (void)simulation::run_topological_simulation(graph); }
        catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "missing/ambiguous submission or unrecognized lane work cannot silently run in parallel");
    }
    graph = pristine;
    plan = {};
    plan.component = "test";
    plan.synthetic_nodes.push_back({.synthetic_id = "invalid", .node = {
        .name = "not CPU", .is_cpu = false, .cpu_task_ready_delay_us = 0}});
    require(!core::validate_dag_mutation_plan(graph, plan).ok(), "non-CPU task declaration is rejected before mutation");
    plan.synthetic_nodes.front().node = {.name="worker with residual gap", .cpu_task_ready_delay_us=0, .cpu_gap_after=2};
    require(!core::validate_dag_mutation_plan(graph, plan).ok(), "worker delays belong to readiness, not a sequential residual gap");
    plan.synthetic_nodes.front().node = {.name="device with residual gap", .is_cpu=false, .cpu_gap_after=2};
    require(!core::validate_dag_mutation_plan(graph, plan).ok(), "device nodes cannot acquire a CPU residual interval");
    require(graph.event_for_node(worker).ts == 0 && graph.event_for_node(worker).arg("correlation_id").empty(),
            "modeled queue participation does not require fabricated trace observations");
}
