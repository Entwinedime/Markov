#include "markov/trace_graph/modules/hicache/runtime/host_removal.hpp"
#include "markov/trace_graph/simulation/cpu_task_queues.hpp"
#include "markov/trace_graph/simulation/topological_simulator.hpp"
#include "markov/trace_graph/modules/hicache/runtime/writes.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_expansion.hpp"
#include <stdexcept>

using namespace markov::trace_graph;
namespace hc = modules::hicache;
namespace {
void require(bool condition, const char * message) {
    if (!condition) throw std::runtime_error(message);
}
void remove_calls(bool external_event, bool adjacent, bool zero_submission = false) {
    using Kind = core::DagEdgeKind;
    core::DagGraph graph;
    const auto add = [&](const char * name, const char * lane, bool cpu, uint64_t at, uint64_t duration,
                         std::optional<uint64_t> ready = {}) {
        return graph.add_synthetic_node({.name = name, .is_cpu = cpu, .lane_key = lane, .duration = duration,
            .cpu_task_ready_delay_us = ready, .observed_point = core::DagObservedPoint{"p", lane, at, 0}});
    };
    const auto entry = add("entry", "main", true, 10, 0);
    const auto submit = add("submit", "main", true, 11, 3);
    const auto zero = add("aten::to", "main", true, 14, 0);
    const auto check = add("AscendCL@aclrtSynchronizeEvent", "main", true, 20, 2);
    const auto exit = add("exit", "main", true, 30, 0);
    const auto second = add("second submit", "main", true, 31, 3);
    const auto end = add("end", "main", true, 40, 0);
    const auto sync = add(external_event ? "AscendCL@aclrtSynchronizeEvent" : "AscendCL@aclrtSynchronizeStream",
                          "main", true, 50, 1);
    const auto final_submit = add("surviving submit", "main", true, 51, 1);
    const auto before = add("earlier device work", "stream", false, 1, 7);
    const auto record = add("EVENT_RECORD", "stream", false, 15, 1);
    const auto second_record = add("EVENT_RECORD", "stream", false, 35, 1);
    const auto after = add("later device work", "stream", false, 60, 4);
    const auto worker = add("runtime task", "worker", true, 14, 4, 2);
    const auto worker2 = add("runtime task", "worker", true, 34, 4, 3);
    const auto worker3 = add("surviving task", "worker", true, 54, 4, 5);
    graph.add_edge(entry, submit, Kind::Sequential);
    graph.add_edge(submit, zero, Kind::Sequential);
    graph.add_edge(zero, check, Kind::Sequential);
    graph.add_edge(check, exit, Kind::Sequential);
    graph.add_edge(exit, second, Kind::Sequential);
    graph.add_edge(second, end, Kind::Sequential);
    graph.add_edge(end, sync, Kind::Sequential);
    graph.add_edge(sync, final_submit, Kind::Sequential);
    graph.add_edge(zero_submission ? zero : submit, worker, Kind::Correlation);
    graph.add_edge(second, worker2, Kind::Correlation);
    graph.add_edge(final_submit, worker3, Kind::Correlation);
    graph.add_edge(worker, worker2, Kind::Sequential);
    graph.add_edge(worker2, worker3, Kind::Sequential);
    graph.add_edge(worker, record, Kind::Correlation);
    graph.add_edge(worker2, second_record, Kind::Correlation);
    graph.add_edge(before, record, Kind::Stream);
    graph.add_edge(record, second_record, Kind::Stream);
    graph.add_edge(second_record, after, Kind::Stream);
    graph.add_edge(record, check, Kind::Sync);
    graph.add_edge(record, sync, Kind::Sync);
    graph.mutable_node(sync).counts_toward_e2e = true;
    graph.mutable_node(entry).cpu_gap_after = 1;
    graph.mutable_node(zero).cpu_gap_after = 6;
    graph.mutable_node(check).cpu_gap_after = 8;
    graph.mutable_node(exit).cpu_gap_after = 1;
    graph.mutable_node(end).cpu_gap_after = 10;
    hc::runtime::HiCacheHostTemplate host, next;
    host.main.status = next.main.status = "ready";
    host.main.interval_start_us = 10; host.main.interval_end_us = 30;
    host.main.owned_node_ids = {submit, check}; host.worker_nodes = {worker};
    if (zero_submission) host.worker_nodes.clear(); // Timing ownership omitted its zero-cost producer.
    host.main.owned_gap_slices = {
        {.owner_node_id = entry, .owned_start_us = 10, .owned_end_us = 11},
        {.owner_node_id = zero, .owned_start_us = 14, .owned_end_us = 20},
        {.owner_node_id = check, .owned_start_us = 22, .owned_end_us = 30}};
    next.main.interval_start_us = 30; next.main.interval_end_us = 40;
    next.main.owned_node_ids = {second}; next.worker_nodes = {worker2};
    next.main.owned_gap_slices = {{.owner_node_id = exit, .owned_start_us = 30, .owned_end_us = 31}};
    std::vector<hc::runtime::HiCacheHostRegion> regions{{entry, exit, &host}};
    if (adjacent) regions.push_back({exit, end, &next});
    core::DagMutationPlan plan;
    bool rejected = false;
    try { plan = hc::runtime::plan_host_removal(hc::patch::HiCacheSourceDagIndex(graph), regions); }
    catch (const std::runtime_error &) { rejected = true; }
    require(rejected == external_event, "only the unowned event consumer must reject removal");
    require(graph.node(submit).active && graph.node(record).active, "planning leaves the graph unchanged");
    if (rejected) return;
    (void)core::apply_dag_mutation_plan(graph, plan);
    require(!graph.node(zero).active && !graph.node(worker).active && !graph.node(record).active,
            "removal includes zero CPU leaves, workers and device events");
    require(graph.node(entry).active && graph.node(exit).active && graph.node(end).active,
            "entry/exit gates survive, including the shared gate of adjacent calls");
    require(graph.node(entry).cpu_gap_after == 0 && graph.node(end).cpu_gap_after == 10
            && graph.node(exit).cpu_gap_after == (adjacent ? 0 : 1),
            "only gaps inside removed calls disappear; outside gaps remain");
    const auto queues = simulation::detail::discover_cpu_task_queues(graph);
    require(queues.tasks.size() == (adjacent ? 1 : 2) && queues.tasks.back().ready_delay_us == 5,
            "surviving worker keeps its queue and ready delay");
    bool frontier = false, order = false;
    for (const auto & edge : graph.edges()) {
        frontier |= edge.active && edge.src == before && edge.dst == sync && edge.kind == Kind::Sync;
        order |= edge.active && edge.src == before && edge.dst == (adjacent ? after : second_record) && edge.kind == Kind::Stream;
    }
    require(frontier && order, "stream sync and ordering retain the live predecessor, not deleted event cost");
    (void)simulation::run_topological_simulation(graph);
    require(graph.node(sync).simulation_start >= graph.node(before).completion_time, "surviving device work still constrains synchronization");
    const auto saved = graph.nodes();
    (void)simulation::run_topological_simulation(graph);
    for (const auto & node : saved)
        if (node.active) require(node.simulation_start == graph.node(node.id).simulation_start
            && node.completion_time == graph.node(node.id).completion_time, "removed region survives static replay");
}
void remove_observed_worker_chain() {
    using Kind = core::DagEdgeKind;
    core::DagGraph graph;
    const auto add = [&](const char * name, const char * lane, bool cpu, uint64_t at, uint64_t duration) {
        return graph.add_synthetic_node({.name = name, .is_cpu = cpu, .lane_key = lane, .duration = duration,
            .observed_point = core::DagObservedPoint{"p", lane, at, 0}});
    };
    const auto entry = add("entry", "main", true, 1, 0);
    const auto submit = add("enqueue", "main", true, 2, 0);
    const auto exit = add("exit", "main", true, 10, 0);
    const auto first = add("aclrtGetStreamAttribute", "worker", true, 3, 2);
    const auto last = add("Node@launch", "worker", true, 5, 2);
    const auto device = add("EVENT_RECORD", "stream", false, 8, 1);
    graph.mutable_event_for_node(submit).cat = "enqueue";
    for (const auto id : {submit, first, last}) graph.mutable_event_for_node(id).set_arg("correlation_id", "42");
    graph.add_edge(entry, submit, Kind::Sequential);
    graph.add_edge(submit, exit, Kind::Sequential);
    graph.add_edge(submit, first, Kind::Correlation);
    graph.add_edge(first, last, Kind::Sequential);
    graph.add_edge(first, last, Kind::Correlation);
    graph.add_edge(last, device, Kind::Correlation);
    hc::runtime::HiCacheHostTemplate host;
    host.main.status = "ready";
    host.main.interval_start_us = 1; host.main.interval_end_us = 10;
    const hc::runtime::HiCacheHostRegion region{entry, exit, &host};
    const auto plan = hc::runtime::plan_host_removal(hc::patch::HiCacheSourceDagIndex(graph), std::span(&region, 1));
    (void)core::apply_dag_mutation_plan(graph, plan);
    require(!graph.node(first).active && !graph.node(last).active && !graph.node(device).active,
            "same-worker correlation steps belong to one observed queue task, not a return to the caller");
    const auto empty = hc::runtime::observe_write_stream_position(hc::patch::HiCacheSourceDagIndex(graph), device, "p", "main", 10);
    require(!empty.before && !empty.after, "a fully removed stream remains an empty resource, not an inactive dependency");
    const auto producer = add("later submit", "main", true, 12, 1);
    const auto later = add("later transfer", "stream", false, 20, 2);
    graph.add_edge(producer, later, Kind::Correlation);
    const hc::patch::HiCacheSourceDagIndex surviving(graph);
    const auto before = hc::runtime::observe_write_stream_position(surviving, device, "p", "main", 10);
    const auto after = hc::runtime::observe_write_stream_position(surviving, device, "p", "main", 15);
    require(!before.before && before.after == later && after.before == later && !after.after,
            "inactive donor identity locates only live endpoints through their CPU submission, not device execution time");
}
void inserted_work_reaches_later_sync(bool bind) {
    using Kind = core::DagEdgeKind;
    core::DagGraph graph;
    const auto cpu = [&](const char * name, uint64_t at) {
        return graph.add_synthetic_node({.name = name, .lane_key = "main", .duration = 1,
            .observed_point = core::DagObservedPoint{"p", "main", at, 0}});
    };
    const auto before = cpu("AscendCL@aclrtSynchronizeStream", 10);
    const auto entry = cpu("insert first", 20);
    const auto middle = cpu("AscendCL@aclrtSynchronizeStream", 30);
    const auto next = cpu("insert second", 40);
    const auto last = cpu("AscendCL@aclrtSynchronizeStream", 50);
    graph.mutable_node(last).counts_toward_e2e = true;
    graph.add_edge(before, entry, Kind::Sequential);
    graph.add_edge(entry, middle, Kind::Sequential);
    graph.add_edge(middle, next, Kind::Sequential);
    graph.add_edge(next, last, Kind::Sequential);
    const auto old = graph.add_synthetic_node({.name = "old record", .is_cpu = false, .lane_key = "copy"});
    const auto event_only = cpu("AscendCL@aclrtSynchronizeEvent", 60);
    const auto discarded = cpu("AscendCL@aclrtSynchronizeStream", 60);
    const auto unrelated = cpu("AscendCL@aclrtSynchronizeStream", 60);
    const auto other = graph.add_synthetic_node({.name = "other resource", .is_cpu = false, .lane_key = "unrelated"});
    graph.add_edge(old, event_only, Kind::Sync);
    graph.add_edge(old, discarded, Kind::Sync);
    graph.add_edge(other, unrelated, Kind::Sync);
    const auto lane = graph.node(old).lane_id;
    for (const auto id : {before, middle, last}) graph.add_edge(old, id, Kind::Sync);
    hc::runtime::HiCacheWriteStreamInsertions insertions;
    if (bind) insertions.bind_synchronizations(graph);
    core::DagMutationPlan removal{.component = "test", .disable_nodes = {old, discarded}};
    (void)core::apply_dag_mutation_plan(graph, removal);
    hc::runtime::HiCacheHostExpansion plan;
    plan.nodes = {{{.name = "new DMA", .is_cpu = false, .lane_key = "copy", .duration = 80}},
                  {{.name = "asynchronous host return", .lane_key = "main"}}};
    plan.host_return = 1;
    plan.streams = {{old, 0, 0}};
    size_t first_done = 0, second_done = 0;
    const hc::patch::HiCacheSourceDagIndex source(graph);
    (void)simulation::run_topological_simulation(graph, {}, [&](size_t id, uint64_t, simulation::FutureDag & future) {
        if (id != entry && id != next) return;
        const auto position = hc::runtime::observe_write_stream_position(source, old, "p", "main", id == entry ? 20 : 40);
        const std::map<size_t, hc::runtime::HiCacheWriteStreamPosition> positions{{lane, position}};
        hc::runtime::HiCacheHostSequence sequence(positions, insertions, future);
        const auto expanded = sequence.append(plan, std::span(&lane, 1));
        (id == entry ? first_done : second_done) = expanded.stream_tails.front();
    });
    require(graph.node(before).simulation_start == 0, "earlier sync never waits on future insertions");
    require((graph.node(middle).simulation_start >= graph.node(first_done).completion_time) == bind,
            "counterexample without resource binding misses newly inserted DMA at the middle sync");
    if (bind) {
        require(graph.node(last).simulation_start >= graph.node(second_done).completion_time,
                "later sync sees the second actual insertion");
        require(graph.node(middle).completion_time < graph.node(second_done).completion_time,
                "middle sync must not wait for the later insertion");
    }
    for (const auto & edge : graph.edges())
        if (edge.active && (edge.src == first_done || edge.src == second_done))
            require(edge.dst != event_only && edge.dst != unrelated && edge.dst != discarded,
                    "new stream work cannot acquire unrelated, event-specific or removed consumers");
    const auto saved = graph.nodes();
    (void)simulation::run_topological_simulation(graph);
    for (const auto & node : saved)
        if (node.active) require(node.simulation_start == graph.node(node.id).simulation_start
            && node.completion_time == graph.node(node.id).completion_time, "new sync dependencies survive static replay");
}
void nested_removal_does_not_restore_old_gaps() {
    core::DagGraph graph;
    const auto add = [&](const char * name, uint64_t at, uint64_t duration, uint64_t gap) {
        const auto id = graph.add_synthetic_node({.name = name, .lane_key = "main", .duration = duration,
            .cpu_gap_after = gap, .observed_point = core::DagObservedPoint{"p", "main", at, 0}});
        return id;
    };
    const std::vector<size_t> nodes{
        add("before", 0, 1, 9), add("outer entry", 10, 0, 1), add("first", 11, 2, 7),
        add("inner entry", 20, 0, 1), add("inner", 21, 4, 5), add("inner exit", 30, 0, 1),
        add("last", 31, 2, 7), add("outer exit", 40, 0, 10), add("after", 50, 1, 0)};
    graph.mutable_node(nodes.back()).counts_toward_e2e = true;
    for (size_t i = 1; i < nodes.size(); ++i) graph.add_edge(nodes[i-1], nodes[i], core::DagEdgeKind::Sequential);
    const auto remove = [&](size_t entry, size_t exit, uint64_t begin, uint64_t end) {
        const hc::patch::HiCacheSourceDagIndex source(graph);
        const auto host = hc::runtime::observe_host_template(source, "p", "main", begin, end);
        const hc::runtime::HiCacheHostRegion region{entry, exit, &host};
        (void)core::apply_dag_mutation_plan(graph, hc::runtime::plan_host_removal(source, std::span(&region, 1)));
    };
    remove(nodes[3], nodes[5], 20, 30);
    require(graph.node(nodes[3]).cpu_gap_after == 0 && !graph.node(nodes[4]).active,
            "inner call removal clears its owned work but retains source coordinates");
    remove(nodes[1], nodes[7], 10, 40);
    require(graph.node(nodes[0]).cpu_gap_after == 9 && graph.node(nodes[7]).cpu_gap_after == 10,
            "outer removal preserves both external nonzero gaps");
    (void)simulation::run_topological_simulation(graph);
    require(graph.node(nodes.back()).completion_time == 21,
            "nested timestamp holes cannot reappear as simulated gap cost");
}
} // namespace

void check_host_removal() {
    nested_removal_does_not_restore_old_gaps();
    inserted_work_reaches_later_sync(false);
    inserted_work_reaches_later_sync(true);
    remove_observed_worker_chain();
    remove_calls(false, false);
    remove_calls(false, true);
    remove_calls(true, false);
    remove_calls(true, true);
    remove_calls(false, false, true);
    remove_calls(false, true, true);
}
