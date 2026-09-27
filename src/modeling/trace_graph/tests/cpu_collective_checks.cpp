#include "hicache_timing_fixture.hpp"
#include "markov/trace_graph/core/dag_builder.hpp"
#include "markov/trace_graph/modules/hicache/cpu_collective_timing.hpp"
#include "markov/trace_graph/modules/hicache/cpu_collectives.hpp"
#include <stdexcept>

using namespace markov::trace_graph;
namespace hc = modules::hicache;
namespace {
void require(bool value, const char * message) {
    if (!value) throw std::runtime_error(message);
}
core::TraceEvent event(const char * name, std::string pid, std::string tid, uint64_t ts, uint64_t dur) {
    core::TraceEvent e;
    e.name = name;
    e.pid = std::move(pid);
    e.tid = std::move(tid);
    e.ts = ts;
    e.dur = dur;
    e.cat = "cpu_op";
    e.source_channel = core::TraceSourceChannel::Torch;
    return e;
}
std::vector<core::TraceEvent> fixture() {
    std::vector<core::TraceEvent> events;
    for (int rank = 0; rank < 2; ++rank) {
        const auto pid = std::to_string(rank + 1);
        events.push_back(event("before", pid, pid, 0, 1));
        events.push_back(event("c10d::allreduce_", pid, pid, 10, 20));
        events.push_back(event("gloo:all_reduce", pid, "worker", 12, 38));
        events.push_back(event("after", pid, pid, 100, 1));
        auto call = event("runtime.cpu_collective", pid, pid, 5, 55);
        call.cat = "runtime_diagnostic";
        call.source_channel = core::TraceSourceChannel::PythonProbe;
        hicache_timing_fixture::set_collective_args(call, rank, 9);
        call.set_arg("sequence_before", rank ? "7" : "3");
        call.set_arg("sequence_after", rank ? "8" : "4");
        events.push_back(std::move(call));
    }
    return events;
}
hc::CpuCollectiveObservation observe(std::vector<core::TraceEvent> events) {
    const auto graph = core::DagBuilder(1).build(std::move(events), 0);
    const auto count = graph.node_count();
    const auto result = hc::observe_cpu_collectives(hc::patch::HiCacheSourceDagIndex(graph));
    require(graph.node_count() == count, "collective observation cannot mutate the graph");
    return result;
}
void check_envelope_calls() {
    auto events = fixture();
    std::erase_if(events, [](const auto & e) { return e.source_channel != core::TraceSourceChannel::PythonProbe; });
    auto source = core::DagBuilder(1).build(std::move(events), 0);
    const auto observed = hc::observe_cpu_collectives(hc::patch::HiCacheSourceDagIndex(source));
    const auto & round = observed.rounds.at(0);
    require(!hc::observe_cpu_collective_timing(source, round).issue.empty(), "missing internal work remains invalid for native timing");
    const auto timing = hc::observe_cpu_all_reduce_envelope(source, round);
    require(timing.issue.empty() && timing.calls.at(0).envelope_remainder_us == 55, "complete envelopes retain an explicit non-native remainder");
    core::DagGraph graph;
    const auto root = graph.add_synthetic_node({ .name = "before query agreement", .counts_toward_e2e = true });
    hc::CpuRankNodes entries, returned;
    for (const auto [rank, delay] : hc::CpuRankTimes{
             { 0,   5 },
             { 1, 105 }
    }) {
        entries[rank] = graph.add_synthetic_node({ .name = "rank entry", .duration = delay });
        graph.mutable_node(entries[rank]).gpu_id = rank;
        graph.add_edge(root, entries[rank], core::DagEdgeKind::Mutation);
    }
    (void)simulation::run_topological_simulation(graph, {}, [&](size_t node, uint64_t, simulation::FutureDag & future) {
        if (node == root) returned = hc::append_cpu_collective(timing, entries, future);
    });
    size_t service_nodes = 0;
    for (const auto & node : graph.nodes()) {
        const auto & event = graph.event_for_node(node.id);
        if (event.cat != "cpu_collective") continue;
        require(event.name == "collective call-envelope remainder", "envelope timing must not invent worker or submission stages");
        ++service_nodes;
    }
    require(service_nodes == 2, "envelope timing has exactly one measured remainder per rank");
    for (const auto [rank, node] : returned)
        require(graph.node(node).completion_time == 160 && graph.node(node).gpu_id == rank
                    && graph.event_for_node(node).arg("timing_evidence") == "call_envelope",
                "late peer shifts the explicit coarse call and retains rank/evidence");
    auto incomplete = round;
    incomplete.calls.pop_back();
    require(!hc::observe_cpu_all_reduce_envelope(source, incomplete).issue.empty(), "partial envelopes cannot invent a peer");
    auto runtime = source.runtime_observations();
    const auto original = runtime;
    runtime.at(round.calls[0].observation).set_arg("async_op", "true");
    source.set_runtime_observations(runtime);
    require(!hc::observe_cpu_all_reduce_envelope(source, round).issue.empty(), "async return is not a barrier");
    runtime = original;
    runtime.at(round.calls[1].observation).ts = 100;
    source.set_runtime_observations(runtime);
    require(hc::observe_cpu_all_reduce_envelope(source, round).issue == "envelope_return_before_peer_entry", "negative remainders are not clamped to zero");
}
} // namespace

void check_cpu_collectives() {
    check_envelope_calls();
    const auto source_graph = core::DagBuilder(1).build(fixture(), 0);
    for (const auto * name :
         { "runtime.hicache.write_policy_check", "runtime.hicache.radix_insert", "runtime.hicache.node_publish", "runtime.hicache.device_restore" }) {
        auto events = fixture();
        auto check = event(name, "1", "1", 61, 10);
        check.cat = "runtime_diagnostic";
        check.source_channel = core::TraceSourceChannel::PythonProbe;
        check.set_arg("node_id", "7");
        check.set_arg("status", "returned");
        events.push_back(check);
        const auto observed = core::DagBuilder(1).build(std::move(events), 0);
        require(observed.node_count() == source_graph.node_count(), "policy check envelope is not extra CPU work");
        require(observed.runtime_observations().size() == source_graph.runtime_observations().size() + 1, "policy check must survive DAG input classification");
        bool found = false;
        for (const auto & observation : observed.runtime_observations())
            if (observation.name == check.name && observation.ts == 61 && observation.dur == 10) found = true;
        require(found, "policy check retains its internal entry and return boundaries");
    }
    const auto source_rounds = hc::observe_cpu_collectives(hc::patch::HiCacheSourceDagIndex(source_graph));
    const auto timing = hc::observe_cpu_collective_timing(source_graph, source_rounds.rounds.front());
    require(timing.issue.empty(), "execution test must use a fully observed communication template");
    {
        core::DagGraph graph;
        const auto root = graph.add_synthetic_node({ .name = "before per-rank submissions", .counts_toward_e2e = true });
        hc::CpuRankNodes entries, returned;
        const hc::CpuRankTimes starts{
            { 0,   5 },
            { 1, 105 }
        };
        for (const auto & [rank, time] : starts) {
            const auto before = graph.add_synthetic_node({ .name = "entry delay", .duration = time });
            entries[rank] = graph.add_synthetic_node({ .name = "local collective entry" });
            graph.mutable_node(entries[rank]).gpu_id = rank;
            graph.add_edge(root, before, core::DagEdgeKind::Mutation);
            graph.add_edge(before, entries[rank], core::DagEdgeKind::Mutation);
        }
        hc::CpuCollectiveExecution execution(timing);
        (void)simulation::run_topological_simulation(graph, {}, [&](size_t id, uint64_t, simulation::FutureDag & future) {
            if (id == root) returned = execution.prepare(entries, future);
            for (const auto & [rank, entry] : entries)
                if (id == entry) (void)execution.submit(rank, entry, future);
        });
        // The late rank enters at 105 us; the observed call remainder is 55 us.
        const uint64_t expected_us = 160;
        size_t submissions = 0;
        for (const auto & node : graph.nodes()) {
            if (graph.event_for_node(node.id).name != "collective submission") continue;
            ++submissions;
            require(node.simulation_start == starts.at(node.gpu_id) + timing.calls.at(node.gpu_id).before_submit,
                    "an early rank submits without waiting for the last peer entry");
        }
        require(submissions == 2, "both execution callbacks must submit their own work");
        for (const auto & [rank, node] : returned) require(graph.node(node).completion_time == expected_us, "incremental submission retains collective timing");
        const auto saved = graph.nodes();
        (void)simulation::run_topological_simulation(graph);
        for (const auto & node : saved)
            require(node.simulation_start == graph.node(node.id).simulation_start && node.completion_time == graph.node(node.id).completion_time,
                    "incremental communication must remain a statically replayable DAG");
    }
    for (const auto starts : {
             hc::CpuRankTimes{   { 0, 5 },   { 1, 5 } },
             hc::CpuRankTimes{   { 0, 5 }, { 1, 105 } },
             hc::CpuRankTimes{ { 0, 105 },   { 1, 5 } }
    }) {
        core::DagGraph graph;
        const auto root = graph.add_synthetic_node({ .name = "before collective" });
        hc::CpuRankNodes entries, returned;
        for (const auto & [rank, time] : starts) {
            entries[rank] = graph.add_synthetic_node({ .name = "entry delay", .duration = time });
            graph.mutable_node(entries[rank]).gpu_id = rank;
            graph.mutable_event_for_node(entries[rank]).pid = std::to_string(rank + 1);
            graph.add_edge(root, entries[rank], core::DagEdgeKind::Mutation);
        }
        const auto endpoint = graph.add_synthetic_node({ .name = "after collective", .counts_toward_e2e = true });
        graph.add_edge(root, endpoint, core::DagEdgeKind::Mutation);
        (void)simulation::run_topological_simulation(graph, {}, [&](size_t id, uint64_t, simulation::FutureDag & future) {
            if (id != root) return;
            returned = hc::append_cpu_collective(timing, entries, future);
            for (const auto & [rank, node] : returned) future.depend(node, endpoint);
        });
        const uint64_t expected_us = std::max(starts.at(0), starts.at(1)) + 55;
        for (const auto & [rank, node] : returned) {
            require(graph.node(node).completion_time == expected_us,
                    "live communication DAG must preserve submission/worker overlap and late-peer dependencies");
            require(graph.node(node).gpu_id == rank && graph.event_for_node(node).pid == std::to_string(rank + 1),
                    "multi-rank generation must preserve rank and PID");
        }
    }
    auto result = observe(fixture());
    require(result.issues.empty() && result.rounds.size() == 1, "common index matches despite different native sequence numbers");
    require(result.rounds.front().calls.size() == 2 && result.rounds.front().calls.front().worker,
            "worker may overlap CPU submission without being serialized or discarded");
    for (const auto * field : { "numel", "role", "scheduler_phase", "request_id" }) {
        auto events = fixture();
        events.back().set_arg(field, "different");
        require(observe(events).issues.contains("collective_signature_mismatch"), "signature conflict cannot form a collective");
    }
    auto events = fixture();
    events.pop_back();
    require(observe(events).issues.contains("collective_membership_mismatch"), "missing participant is not a complete round");
    events = fixture();
    events.back().set_arg("rank", "0");
    require(observe(events).issues.contains("collective_membership_mismatch"), "duplicate rank is not membership coverage");
    events = fixture();
    events.back().set_arg("async_op", "true");
    require(observe(events).issues.contains("not_completed_synchronous_call"), "async return is not completion");
    events = fixture();
    events.back().set_arg("sequence_after", "7");
    require(observe(events).issues.contains("not_completed_synchronous_call"), "coalesced call does not invent native work");
    events = fixture();
    events[2].name = "not a collective";
    require(observe(events).issues.contains("missing_worker"), "missing worker cannot be borrowed from another rank");
    events = fixture();
    auto duplicate = events[2];
    duplicate.tid = "other worker";
    events.push_back(duplicate);
    require(observe(events).issues.contains("nonunique_worker"), "multiple matching workers cannot be assigned by ordinal");
    events = fixture();
    auto other_group = events[4];
    other_group.set_arg("group", "37");
    events.push_back(other_group);
    require(observe(events).issues.at("worker_claimed_by_multiple_calls") == 2, "cross-group worker reuse must be rejected");
    events = fixture();
    other_group.tid = "background";
    events.push_back(other_group);
    result = observe(events);
    require(result.issues.contains("incomplete_cpu_interval") && !result.issues.contains("worker_claimed_by_multiple_calls"),
            "unobserved background caller does not steal a main-thread worker merely by time overlap");
    require(result.rounds.front().calls.front().issue.empty(), "background coverage failure remains separate from the proven main call");
    events = fixture();
    events.push_back(event("unexplained work", "1", "1", 40, 5));
    require(observe(events).issues.contains("unexplained_call_work"), "extra real CPU work cannot be silently removed as waiting");
}
