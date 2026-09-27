#include "markov/trace_graph/core/dag_mutation.hpp"
#include "markov/trace_graph/simulation/topological_simulator.hpp"
#include <stdexcept>
#include <vector>
using namespace markov::trace_graph;
namespace {
void require(bool ok, const char * message) {
    if (!ok) throw std::runtime_error(message);
}
size_t node(core::DagGraph & g, const char * name, uint64_t duration, bool endpoint = false) {
    return g.add_synthetic_node({ .name = name, .duration = duration, .counts_toward_e2e = endpoint });
}
void future_dependency_invalidates_ready_entry() {
    core::DagGraph g;
    const auto source = node(g, "source", 0), consumer = node(g, "consumer", 2, true);
    // consumer is already ready at time zero when source adds its prerequisite.
    g.mutable_node(source).gpu_id = 3;
    std::vector<size_t> visits;
    size_t work = 0, finish = 0;
    const auto result = simulation::run_topological_simulation(
        g,
        [&](size_t id, uint64_t, uint64_t duration) {
            visits.push_back(id);
            return duration;
        },
        [&](size_t id, uint64_t, simulation::FutureDag & future) {
            if (id == source) {
                work = future.append({ .name = "query", .duration = 7 });
                finish = future.append({ .name = "completion" });
                future.depend(work, finish);
                future.depend(finish, consumer);
                future.depend(finish, consumer); // Declarative duplicate, not a second wait.
            }
            if (id == finish) {
                const auto released = future.append({ .name = "release", .duration = 3 });
                future.depend(released, consumer);
                future.depend(source, released); // Completed predecessor is accounted immediately.
            }
        });
    require(result.e2e_us == 12 && visits.size() == 5 && result.processed_nodes == 5, "future work must delay an already queued consumer exactly once");
    require(g.node(work).gpu_id == 3 && !g.node(work).counts_toward_e2e, "new work inherits rank, not endpoint eligibility");
    const auto saved = g.nodes();
    require(simulation::run_topological_simulation(g).e2e_us == 12, "materialized graph must retain dynamic dependencies");
    for (const auto & n : saved)
        require(n.simulation_start == g.node(n.id).simulation_start && n.completion_time == g.node(n.id).completion_time,
                "static replay of generated work must match every node");
}
void pending_cpu_task_can_gain_a_dependency() {
    core::DagGraph g;
    const auto submitted = node(g, "submit", 0), decision = node(g, "decision", 0);
    const auto task =
        g.add_synthetic_node({ .name = "native worker", .lane_key = "worker", .duration = 2, .counts_toward_e2e = true, .cpu_task_ready_delay_us = 10 });
    g.add_edge(submitted, task, core::DagEdgeKind::Correlation);
    const auto result = simulation::run_topological_simulation(g, {}, [&](size_t id, uint64_t, simulation::FutureDag & future) {
        if (id == decision) {
            const auto work = future.append({ .name = "required work", .duration = 7 });
            future.depend(work, task);
        }
    });
    require(result.e2e_us == 19 && result.cpu_task_count == 1, "native task must discard its old start and retain its measured ready delay");
    require(simulation::run_topological_simulation(g).e2e_us == 19, "native CPU queue order survives expansion and materialization");
}
void reject_history_and_cycles() {
    for (const bool running : { false, true }) {
        core::DagGraph g;
        const auto a = node(g, "earlier", running ? 10 : 0), b = node(g, "later", 0, true);
        bool rejected = false;
        try {
            (void)simulation::run_topological_simulation(g, {}, [&](size_t id, uint64_t, simulation::FutureDag & future) {
                if (id == b) future.depend(b, a);
            });
        }
        catch (const std::invalid_argument &) {
            rejected = true;
        }
        require(rejected, "cannot add a prerequisite to completed or running work");
    }
    core::DagGraph g;
    const auto begin = node(g, "begin", 0), endpoint = node(g, "endpoint", 0, true);
    g.add_edge(begin, endpoint, core::DagEdgeKind::Mutation);
    bool rejected = false;
    try {
        (void)simulation::run_topological_simulation(g, {}, [&](size_t id, uint64_t, simulation::FutureDag & future) {
            if (id != begin) return;
            const auto a = future.append({ .name = "a" }), b = future.append({ .name = "b" });
            future.depend(a, b);
            future.depend(b, a);
            future.depend(b, endpoint);
        });
    }
    catch (const std::runtime_error &) {
        rejected = true;
    }
    require(rejected, "a cycle in generated work must fail, never report a partial E2E");
}
void reject_cyclic_mutation_before_apply() {
    core::DagGraph graph;
    const auto prefix = node(graph, "prefix", 1), a = node(graph, "a", 2), b = node(graph, "b", 3);
    graph.add_edge(prefix, a, core::DagEdgeKind::Mutation);
    graph.add_edge(a, b, core::DagEdgeKind::Mutation);
    core::DagMutationPlan plan;
    plan.component = "cycle_check";
    plan.set_node_durations.push_back({ .node_id = a, .duration = 20 });
    plan.add_edges.push_back({ .src = core::DagNodeRef::existing(b), .dst = core::DagNodeRef::existing(a) });
    bool rejected = false;
    try {
        (void)core::apply_dag_mutation_plan(graph, plan);
    }
    catch (const core::DagMutationValidationError & error) {
        rejected = true;
        require(error.report().cycle_nodes == std::vector<size_t>{ a, b }, "cycle report excludes the consumed acyclic prefix");
    }
    require(rejected && graph.edge_count() == 2 && graph.node(a).duration == 2, "cyclic plan must fail before applying either its costs or dependencies");
}
} // namespace
void check_dynamic_execution() {
    future_dependency_invalidates_ready_entry();
    pending_cpu_task_can_gain_a_dependency();
    reject_history_and_cycles();
    reject_cyclic_mutation_before_apply();
}
