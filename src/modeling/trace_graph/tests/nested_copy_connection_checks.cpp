#include "markov/trace_graph/core/dag_builder.hpp"
#include "markov/trace_graph/simulation/topological_simulator.hpp"
#include <algorithm>
#include <stdexcept>

using namespace markov::trace_graph;
namespace {
void require(bool value, const char * message) {
    if (!value) throw std::runtime_error(message);
}
core::DagGraph nested_copy(bool ambiguous, bool crosses_boundary) {
    std::vector<core::TraceEvent> events;
    const auto add = [&](const char * name, const char * tid, uint64_t at, uint64_t duration, const char * cat, const char * key, const char * id) {
        core::TraceEvent event;
        event.source_channel = core::TraceSourceChannel::Torch;
        event.name = name;
        event.pid = "worker";
        event.tid = tid;
        event.ts = at;
        event.dur = duration;
        event.cat = cat;
        event.set_arg(key, id);
        events.push_back(std::move(event));
    };
    add("enqueue", "main", 100, 5, "enqueue", "correlation_id", "job");
    add("dequeue", "runtime", 110, 60, "dequeue", "correlation_id", "job");
    add("AscendCL@aclnnCat", "runtime", 120, 40, "", "connection_id", "outer");
    add("AscendCL@aclrtGetStreamAttribute", "runtime", 122, 2, "", "connection_id", "attribute");
    add("AscendCL@aclrtMemcpyAsync", "runtime", crosses_boundary ? 155 : 130, 10, "", "connection_id", "inner");
    if (ambiguous) add("AscendCL@aclrtMemcpy2dAsync", "runtime", 145, 5, "", "connection_id", "other");
    add("MEMCPY_ASYNC", "device", 200, 7, "", "connection_id", "outer");
    events.back().set_arg("Physic Stream Id", "7");
    add("MEMCPY_ASYNC", "device", 210, 3, "", "connection_id", "inner");
    events.back().set_arg("Physic Stream Id", "7");
    return core::DagBuilder(1).build(std::move(events), 0);
}
bool connected(const core::DagGraph & graph, const std::string & connection) {
    for (const auto & edge : graph.edges()) {
        if (!edge.active || edge.kind != core::DagEdgeKind::Correlation || !graph.node(edge.src).is_cpu || graph.node(edge.dst).is_cpu) continue;
        if (graph.event_for_node(edge.dst).arg("connection_id") == connection) {
            require(graph.event_for_node(edge.src).name == "AscendCL@aclrtMemcpyAsync", "attribute queries cannot own device copies");
            return true;
        }
    }
    return false;
}
} // namespace

void check_nested_copy_connections() {
    auto graph = nested_copy(false, false);
    require(connected(graph, "outer") && connected(graph, "inner"), "nested memcpy keeps its own and enclosing device identities");
    require(std::ranges::none_of(graph.events(), [](const auto & event) { return event.name == "AscendCL@aclnnCat"; }),
            "retaining identity must not reintroduce the wrapper cost");
    uint64_t cpu_cost = 0;
    size_t submission = 0;
    for (const auto & node : graph.nodes()) {
        if (node.is_cpu) cpu_cost += node.duration;
        if (graph.event_for_node(node.id).name == "enqueue") submission = node.id;
    }
    require(cpu_cost == 17, "CPU leaf costs stay unchanged when restoring device causality");
    (void)simulation::run_topological_simulation(graph);
    const auto original = graph.nodes();
    graph.mutable_node(submission).duration += 45;
    (void)simulation::run_topological_simulation(graph);
    for (const auto & node : graph.nodes())
        if (!node.is_cpu) require(node.completion_time == original[node.id].completion_time + 45, "both copies follow changed main-thread submission");
    require(!connected(nested_copy(true, false), "outer"), "two matching nested submissions remain ambiguous");
    require(!connected(nested_copy(false, true), "outer"), "a boundary-crossing memcpy cannot inherit wrapper identity");
}
