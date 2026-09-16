/** Native waits remain dependencies when the device profiler omits a marker. */
#include "markov/trace_graph/core/dag_builder.hpp"
#include "markov/trace_graph/core/dag_mutation.hpp"
#include "markov/trace_graph/simulation/topological_simulator.hpp"
#include <algorithm>
#include <stdexcept>

using namespace markov::trace_graph;

namespace {
void require(bool condition, const char * message) {
    if (!condition) throw std::runtime_error(message);
}

core::TraceEvent event(std::string name, std::string tid, uint64_t ts, uint64_t duration,
                      std::string connection = {}, bool device = false) {
    core::TraceEvent result;
    result.source_channel = core::TraceSourceChannel::Torch;
    result.name = std::move(name); result.pid = "1"; result.tid = std::move(tid);
    result.ts = ts; result.dur = duration; result.cat = device ? "Kernel" : "cpu_op";
    if (!connection.empty()) result.set_arg("connection_id", connection);
    if (device) result.set_arg("Physic Stream Id", result.tid);
    return result;
}

std::vector<core::TraceEvent> fixture(bool tail = false) {
    auto record = event("AscendCL@aclrtRecordEvent", "1", 2, 1, "record");
    record.set_arg("Event Id", "ready"); record.set_arg("Raw Stream", "producer");
    auto wait = event("AscendCL@aclrtStreamWaitEvent", "2", 20, 2, "wait");
    wait.set_arg("Event Id", "ready"); wait.set_arg("Raw Stream", "consumer");
    auto prior = event("prior launch", "2", 18, 1, "prior");
    prior.set_arg("Raw Stream", "consumer");
    std::vector<core::TraceEvent> events{
        event("copy launch", "1", 0, 1, "copy"), event("copy", "10", 1, 9, "copy", true),
        record, event("EVENT_RECORD", "10", 10, 1, "record", true),
        event("CPU preparation", "2", 0, 18), prior, event("prior", "11", 19, 2, "prior", true), wait};
    if (tail) {
        auto sync = event("AscendCL@aclrtSynchronizeStream", "2", 25, 100);
        sync.set_arg("Raw Stream", "consumer"); events.push_back(sync);
    } else {
        auto launch = event("consume launch", "2", 23, 1, "consume");
        launch.set_arg("Raw Stream", "consumer"); events.push_back(launch);
        events.push_back(event("consume", "11", 24, 3, "consume", true));
    }
    return events;
}

size_t find(const core::DagGraph & graph, const std::string & name) {
    const auto found = std::ranges::find_if(graph.nodes(), [&](const auto & n) { return graph.event_for_node(n.id).name == name; });
    if (found == graph.nodes().end()) throw std::runtime_error("missing node: " + name);
    return found->id;
}

size_t logical_waits(const core::DagGraph & graph) {
    return std::ranges::count_if(graph.nodes(), [&](const auto & n) {
        return n.active && graph.event_for_node(n.id).name == "logical_event_wait";
    });
}
}

void check_native_stream_waits() {
    auto graph = core::DagBuilder(1).build(fixture(), 0);
    require(logical_waits(graph) == 1, "a missing device wait gets one zero-cost dependency");
    require(simulation::run_topological_simulation(graph).e2e_us == 27, "already-completed wait leaves source timing unchanged");
    const auto prior = graph.node(find(graph, "prior")).completion_time;
    graph.set_node_duration(find(graph, "copy"), 100);
    (void)simulation::run_topological_simulation(graph);
    require(graph.node(find(graph, "consume")).completion_time == 105, "consumer follows slowed producer record at 102 us");
    require(graph.node(find(graph, "prior")).completion_time == prior, "wait must not delay previously submitted work");
    require(graph.node(find(graph, "AscendCL@aclrtStreamWaitEvent")).completion_time == 22, "host wait remains asynchronous");
    const auto join = find(graph, "logical_event_wait");
    require(graph.node(join).duration == 0 && !graph.node(join).counts_toward_e2e, "logical join adds no fixed cost or business endpoint");
    core::DagMutationPlan removal{.component = "remove_wait"};
    removal.disable_nodes.push_back(join);
    (void)core::apply_dag_mutation_plan(graph, removal);
    (void)simulation::run_topological_simulation(graph);
    require(graph.node(find(graph, "consume")).completion_time == 27, "removing a logical wait retains original device stream order");

    auto tail = core::DagBuilder(1).build(fixture(true), 0);
    tail.set_node_duration(find(tail, "copy"), 100);
    (void)simulation::run_topological_simulation(tail);
    require(tail.node(find(tail, "AscendCL@aclrtSynchronizeStream")).completion_time == 112,
            "stream synchronization observes a logical wait even without a later device kernel");

    auto pair = fixture();
    auto second = pair[7]; second.ts = 22; second.dur = 1; second.set_arg("connection_id", "second wait");
    pair.push_back(second);
    auto paired = core::DagBuilder(1).build(pair, 0);
    require(logical_waits(paired) == 2, "K/V waits remain independently owned");
    paired.set_node_duration(find(paired, "copy"), 100);
    require(simulation::run_topological_simulation(paired).e2e_us == 105, "repeated zero-cost waits cannot double-charge an event");

    auto explicit_events = fixture();
    explicit_events.push_back(event("EVENT_WAIT", "11", 22, 1, "wait", true));
    auto explicit_graph = core::DagBuilder(1).build(explicit_events, 0);
    require(logical_waits(explicit_graph) == 0, "an observed device wait is not duplicated");
    explicit_graph.set_node_duration(find(explicit_graph, "copy"), 100);
    (void)simulation::run_topological_simulation(explicit_graph);
    require(explicit_graph.node(find(explicit_graph, "consume")).completion_time >= 105, "existing device waits keep their record dependency");

    auto reuse = fixture();
    auto next_record = reuse[2]; next_record.ts = 25; next_record.set_arg("connection_id", "next record");
    reuse.push_back(next_record); reuse.push_back(event("EVENT_RECORD", "10", 40, 1, "next record", true));
    auto reused = core::DagBuilder(1).build(reuse, 0);
    const auto later = std::ranges::find_if(reused.nodes(), [&](const auto & n) {
        return !n.is_cpu && reused.event_for_node(n.id).arg("connection_id") == "next record";
    })->id;
    reused.set_node_duration(later, 500);
    (void)simulation::run_topological_simulation(reused);
    require(reused.node(find(reused, "consume")).completion_time == 27, "later event reuse does not retarget an earlier wait");

    for (const auto key : {"Event Id", "Raw Stream"}) {
        auto missing = fixture(); missing[7].set_arg(key, "unknown");
        auto incomplete = core::DagBuilder(1).build(missing, 0);
        require(logical_waits(incomplete) == 0, "missing event or stream evidence is not guessed");
        require(incomplete.event_for_node(find(incomplete, "AscendCL@aclrtStreamWaitEvent")).arg("stream_wait_binding").starts_with("missing_"),
                "unbound waits retain a visible reason");
    }
    auto overlap = fixture(); overlap[7].tid = "3"; overlap[8].ts = 21; overlap[8].dur = 3;
    auto ambiguous = core::DagBuilder(1).build(overlap, 0);
    require(logical_waits(ambiguous) == 0, "overlapping host submissions do not prove a wait boundary");

    auto conflict = fixture(); conflict[2].set_arg("Raw Stream", "consumer");
    require(logical_waits(core::DagBuilder(1).build(conflict, 0)) == 0, "one raw stream cannot silently alias two device lanes");
    auto unknown = fixture(); unknown[5].set_arg("connection_id", "unmatched");
    require(logical_waits(core::DagBuilder(1).build(unknown, 0)) == 0, "an unanchored neighboring device node leaves the boundary unproven");
    auto reversed = fixture(); reversed[6].ts = 30;
    require(logical_waits(core::DagBuilder(1).build(reversed, 0)) == 0, "device order conflicting with host submissions is not guessed");
}
