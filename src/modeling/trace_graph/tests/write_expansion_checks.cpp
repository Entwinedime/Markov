#include "hicache_timing_fixture.hpp"
#include "markov/trace_graph/core/dag_mutation.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_expansion.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_calibration.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_confirmations.hpp"
#include "markov/trace_graph/modules/hicache/runtime/load_execution.hpp"
#include "markov/trace_graph/modules/hicache/patch/layer_wait_insertion.hpp"
#include <nlohmann/json.hpp>
#include <stdexcept>

using namespace hicache_timing_fixture;
namespace {
void host_and_worker_use_only_measured_cpu_service() {
    core::DagGraph graph;
    const auto add = [&](const char * lane, uint64_t at, uint64_t duration) {
        const auto id = graph.add_synthetic_node({
            .name = lane,
            .lane_key = lane,
            .duration = duration,
            .observed_point = core::DagObservedPoint{ "p", lane, at, 0 }
        });
        graph.mutable_node(id).kind = core::DagNodeKind::TraceEvent;
        return id;
    };
    const auto main = add("main", 100, 10), tail = add("main", 130, 7);
    const auto first = add("worker", 100, 4);
    const auto last = add("worker", 110, 6);
    simulation::detail::CpuTaskQueues queues;
    queues.tasks.push_back({ .first = first, .last = last, .submission = main, .queue = 0, .ready_delay_us = 3 });
    queues.node_task.assign(graph.nodes().size(), core::DagNode::kNoNode);
    queues.node_task[first] = queues.node_task[last] = 0;
    runtime::HiCacheHostTemplate host;
    host.main.owned_node_ids = { main, tail };
    host.main.owned_gap_slices.push_back({ .owner_node_id = main, .gap_start_us = 110, .gap_end_us = 120, .owned_start_us = 110, .owned_end_us = 120 });
    host.worker_nodes = { first, last };
    const auto build = [&] { return runtime::prepare_host_expansion(patch::HiCacheSourceDagIndex(graph), queues, host); };
    const auto raw = build();
    require(raw.nodes.at(raw.source_nodes.at(first)).work.duration == 16, "unmeasured worker preserves its events and internal gap");
    graph.cpu_service_cost().add({ "p", "main" }, { 100, 120, 8 });
    graph.cpu_service_cost().add({ "p", "worker" }, { 100, 116, 8 });
    const auto measured = build();
    const auto & job = measured.nodes.at(measured.source_nodes.at(first)).work;
    require(job.duration == 8, "worker events and internal gaps must use the same measured CPU budget");
    require(job.cpu_task_ready_delay_us == 3, "queue readiness is not worker CPU service");
    uint64_t main_budget = 0;
    for (const auto & node : measured.nodes)
        if (node.work.lane_key == "main") main_budget += node.work.duration + node.work.cpu_gap_after;
    require(main_budget == 15 && measured.nodes.at(measured.source_nodes.at(tail)).work.duration == 7,
            "main service conserves the measured budget without stripping unmeasured work");
}
void independent_host_resources_are_rebound(bool compute_release = false) {
    core::DagGraph calibration, base;
    const auto add = [](core::DagGraph & graph, const char * lane, bool cpu, int rank, std::optional<uint64_t> ready = {}) {
        return graph.add_synthetic_node({
            .name = "resource",
            .is_cpu = cpu,
            .lane_key = lane,
            .duration = 1,
            .cpu_task_ready_delay_us = ready,
            .observed_point = core::DagObservedPoint{ "pid", lane, 100, rank }
        });
    };
    const auto old_main = add(calibration, "old main", true, 0);
    const auto old_worker = add(calibration, "old worker", true, 0, 0);
    const auto old_stream = add(calibration, "old D2H", false, 0);
    calibration.add_edge(old_main, old_worker, core::DagEdgeKind::Correlation);
    (void)add(base, "unrelated", true, 2);
    const auto main = add(base, "new main", true, 2);
    const auto worker = add(base, "new worker", true, 2, 0);
    const auto stream = add(base, "new D2H", false, 2);
    base.add_edge(main, worker, core::DagEdgeKind::Correlation);
    simulation::detail::CpuTaskQueues resource_queues;
    resource_queues.tasks.push_back({worker, worker, main, 0});
    require(runtime::find_host_worker_resource(base, resource_queues, main, "resource") == worker,
            "worker identity can be witnessed outside the confirmation envelope");
    require(!runtime::find_host_worker_resource(base, resource_queues, main, "unobserved"),
            "unobserved submissions cannot borrow an arbitrary queue");
    const auto other_main = add(base, "other main", true, 2);
    require(!runtime::find_host_worker_resource(base, resource_queues, other_main, "resource"),
            "another main lane cannot donate a worker witness");
    const auto other_rank_worker = add(base, "other rank worker", true, 3);
    resource_queues.tasks = {{other_rank_worker, other_rank_worker, main, 0}};
    require(!runtime::find_host_worker_resource(base, resource_queues, main, "resource"),
            "another rank cannot donate a worker witness");
    resource_queues.tasks = {{worker, worker, main, 0}, {worker, worker, main, 0}};
    require(runtime::find_host_worker_resource(base, resource_queues, main, "resource") == worker,
            "repeated evidence on one lane is unambiguous");
    const auto second_worker = add(base, "second worker", true, 2);
    const auto another_submission = base.add_synthetic_node({.name = "another submission", .is_cpu = true,
        .lane_key = "new main", .duration = 0, .observed_point = core::DagObservedPoint{"pid", "new main", 100, 2}});
    auto evidence_queues = resource_queues;
    evidence_queues.tasks.push_back({worker, worker, another_submission, 0});
    const nlohmann::json evidence{{"worker|opaque", {"resource", "another submission"}}};
    std::map<std::string, size_t> witnessed{{"main", main}};
    runtime::bind_calibration_workers(evidence, base, evidence_queues, witnessed);
    require(witnessed.at("worker|opaque") == worker, "all original submissions bind the same base queue");
    for (const bool missing : {false, true}) {
        auto invalid_queues = resource_queues;
        if (!missing) invalid_queues.tasks.push_back({second_worker, second_worker, another_submission, 0});
        auto invalid_roles = std::map<std::string, size_t>{{"main", main}};
        hicache_timing_fixture::require_throws<std::runtime_error>([&] { runtime::bind_calibration_workers(evidence, base, invalid_queues, invalid_roles); },
                                                                   "missing or split worker evidence cannot supply a calibration resource");
    }
    resource_queues.tasks.push_back({second_worker, second_worker, main, 1});
    hicache_timing_fixture::require_throws<std::runtime_error>([&] { (void)runtime::find_host_worker_resource(base, resource_queues, main, "resource"); },
                                                               "same submission mapped to two worker lanes must be rejected");
    runtime::HiCacheWriteExpansion plan;
    plan.nodes = {{{.name = "enqueue", .lane_key = "old main", .duration = 3, .cpu_gap_after = 2}},
                  {{.name = "return", .lane_key = "old main"}},
                  {{.name = "job", .lane_key = "old worker", .duration = 5, .cpu_task_ready_delay_us = 1}, 0, old_worker},
                  {{.name = "copy", .is_cpu = false, .lane_key = "old D2H", .duration = 7}}};
    plan.edges = {{0, 1, core::DagEdgeKind::Sequential}, {2, 3, core::DagEdgeKind::Mutation}};
    plan.host_return = 1;
    plan.streams = {{old_stream, 3, 3}};
    plan.source_nodes = {{old_main, 0}, {old_worker, 2}, {old_stream, 3}};
    plan.payload = {{3, 7}}; plan.payload_bytes = 7;
    plan.write_start = plan.completion = 3;
    const std::map<std::string, size_t> old_roles{{"main", old_main}, {"worker", old_worker}, {"payload", old_stream}};
    const std::map<std::string, size_t> new_roles{{"main", main}, {"worker", worker}, {"payload", stream}};
    const auto encoded = runtime::export_write_template(calibration, plan, old_roles);
    const auto queues = simulation::detail::discover_cpu_task_queues(base);

    auto missing_worker_roles = new_roles;
    missing_worker_roles.erase("worker");
    bool missing_worker = false;
    try { (void)runtime::import_host_template(encoded, base, queues, missing_worker_roles); }
    catch (const std::runtime_error & error) {
        missing_worker = std::string(error.what()) == "Independent host template lacks a base resource: worker";
    }
    require(missing_worker, "missing worker must identify its role, not borrow another resource");
    hicache_timing_fixture::require_throws<std::runtime_error>([&] { (void)runtime::host_resource_roles(calibration, plan, old_main); },
                                                               "device work cannot silently become a CPU-only host template");
    const auto compute_roles = runtime::host_resource_roles(calibration, plan, old_main, old_stream);
    const std::map<std::string, size_t> base_compute_roles{{"main", main}, {"worker|resource", worker}, {"compute", stream}};
    auto compute_plan = static_cast<runtime::HiCacheHostExpansion>(plan);
    compute_plan.waits.push_back({old_stream, 1});
    const auto compute_json = runtime::export_host_template(calibration, compute_plan, compute_roles);
    const auto compute_import = runtime::import_host_template(compute_json, base, queues, base_compute_roles);
    require(runtime::export_host_template(base, compute_import, base_compute_roles) == compute_json
                && compute_import.streams.front().source_node == stream && compute_import.waits.front().source_node == stream,
            "compute work and stream waits retain their roles across graphs");
    for (const auto invalid : {old_main, add(calibration, "other compute", false, 0), add(calibration, "other rank", false, 1)}) {
        hicache_timing_fixture::require_throws<std::runtime_error>([&] { (void)runtime::host_resource_roles(calibration, compute_plan, old_main, invalid); },
                                                                   "compute witness must match device type, lane and rank");
    }
    runtime::HiCacheHostExpansion acknowledgement;
    acknowledgement.nodes = {{{.name = "ack", .lane_key = "old main", .duration = 3}},
                             {{.name = "ack return", .lane_key = "old main"}}};
    acknowledgement.edges = {{0, 1, core::DagEdgeKind::Sequential}};
    acknowledgement.host_return = 1;
    acknowledgement.event_waits = {{old_stream, 0}};
    const auto host_json = runtime::export_host_template(calibration, acknowledgement, old_roles);
    const auto host_import = runtime::import_host_template(host_json, base, queues, new_roles);
    require(!host_json.contains("payload") && !host_json.contains("completion")
            && runtime::export_host_template(base, host_import, new_roles) == host_json,
            "ACK host template roundtrips without invented DMA payload fields");
    require(host_import.event_waits.size() == 1 && host_import.event_waits[0].source_node == core::DagNode::kNoNode,
            "ACK import retains its wait but requires an explicit target event completion");
    acknowledgement.nodes.push_back(plan.nodes[2]);
    acknowledgement.source_nodes = {{old_main, 0}, {old_worker, 2}};
    const auto host_roles = runtime::host_resource_roles(calibration, acknowledgement, old_main);
    acknowledgement.nodes[0].work.name = "different synthetic prefix: enqueue";
    require(runtime::host_resource_roles(calibration, acknowledgement, old_main) == host_roles
            && host_roles.at("main") == old_main && host_roles.at("worker|resource") == old_worker,
            "host roles use original submission identity, not generated labels");
    hicache_timing_fixture::require_throws<std::runtime_error>([&] { (void)runtime::host_resource_roles(calibration, acknowledgement, old_worker); },
                                                               "worker lane cannot replace the confirmation main lane");
    require(encoded.dump().find("old main") == std::string::npos && encoded.dump().find("old worker") == std::string::npos,
            "portable template contains resource roles rather than calibration lane identities");
    auto wrong = new_roles;
    wrong["worker"] = main;
    bool rejected = false;
    try { (void)runtime::import_write_template(encoded, base, queues, wrong); }
    catch (const std::runtime_error &) { rejected = true; }
    require(rejected && plan.nodes[0].work.lane_key == "old main", "failed rebinding leaves the calibration template intact");
    auto imported = runtime::import_write_template(nlohmann::json::parse(encoded.dump()), base, queues, new_roles);
    require(runtime::export_write_template(base, imported, new_roles) == encoded,
            "serialization roundtrip preserves local topology, work, gaps, queue delay and payload geometry");
    plan = std::move(imported);
    require(plan.source_nodes.empty() && plan.streams[0].source_node == stream && plan.nodes[2].queue_member == worker,
            "all external ids refer to the base, not numerically similar calibration nodes");
    require(plan.nodes[0].work.cpu_gap_after == 2 && plan.nodes[2].work.duration == 5
            && plan.nodes[2].work.cpu_task_ready_delay_us == 1 && plan.nodes[2].submission == 0,
            "rebinding preserves measured work, residual delay and local submission indices");
    const auto site = add(base, "new main", true, 2);
    const auto consumer = add(base, "new main", true, 2);
    base.mutable_node(consumer).counts_toward_e2e = true;
    base.add_edge(site, consumer, core::DagEdgeKind::Sequential);
    runtime::HiCacheExpandedHost expanded{};
    std::vector<runtime::HiCacheWriteStreamPosition> positions{{stream, {}}};
    if (compute_release) {
        base.set_node_duration(stream, 40);
        positions.push_back({stream, {}});
    }
    const auto result = simulation::run_topological_simulation(base, {}, [&](size_t id, uint64_t, simulation::FutureDag & future) {
        if (id == site) {
            expanded = runtime::expand_host(compute_release ? compute_import : plan, positions, future);
            future.depend(expanded.nodes[3], consumer);
            future.depend(expanded.host_return, consumer);
        }
    });
    require(result.cpu_queue_count == 1 && result.cpu_task_count == 2, "imported job joins the actual base FIFO");
    require(base.node(expanded.nodes[2]).gpu_id == 2, "imported worker uses base rank rather than calibration rank");
    if (compute_release) {
        require(base.node(expanded.host_return).simulation_start >= base.node(stream).completion_time,
                "imported release return must preserve its wait for the base compute frontier");
        require(base.node(expanded.nodes[3]).simulation_start >= base.node(stream).completion_time,
                "imported allocator compute must join the base stream after prior work");
    }
    require_static_replay(base, "cross-graph template expansion survives static replay");
}
void lifecycle_write_precedes_return(bool late_insertion, uint64_t busy_us) {
    core::DagGraph graph;
    const auto prefix = graph.add_synthetic_node({ .name = "lifecycle prefix", .lane_key = "main", .duration = 10 });
    const auto submit_site = graph.add_synthetic_node({ .name = "publication site", .lane_key = "main" });
    const auto suffix = graph.add_synthetic_node({ .name = "lifecycle suffix", .lane_key = "main", .duration = 5 });
    const auto returned = graph.add_synthetic_node({ .name = "lifecycle returned", .lane_key = "main" });
    const auto next = graph.add_synthetic_node({ .name = "next CPU", .lane_key = "main", .duration = 1, .counts_toward_e2e = true });
    const auto prior = graph.add_synthetic_node({ .name = "prior D2H", .is_cpu = false, .lane_key = "write", .duration = busy_us });
    graph.add_edge(prefix, submit_site, core::DagEdgeKind::Sequential);
    graph.add_edge(submit_site, suffix, core::DagEdgeKind::Sequential);
    graph.add_edge(suffix, returned, core::DagEdgeKind::Sequential);
    graph.add_edge(returned, next, core::DagEdgeKind::Sequential);
    runtime::HiCacheWriteExpansion plan;
    // Synthetic contract, not a measured cost or a production calibration.
    plan.nodes = {{{ .name = "submit", .lane_key = "main", .duration = 3 }},
                  {{ .name = "host return", .lane_key = "main", .duration = 2 }},
                  {{ .name = "copy", .is_cpu = false, .lane_key = "write" }}};
    plan.edges = {{0, 1, core::DagEdgeKind::Sequential}, {0, 2, core::DagEdgeKind::Mutation}};
    plan.streams = {{prior, 2, 2}};
    plan.payload = {{2, 1}};
    plan.payload_bytes = 1;
    plan.host_return = 1;
    plan.write_start = plan.completion = 2;
    const std::vector<runtime::HiCacheWriteStreamPosition> positions{{prior, {}}};
    runtime::HiCacheExpandedWrite expanded{};
    (void)simulation::run_topological_simulation(graph, {}, [&](size_t node, uint64_t, simulation::FutureDag & future) {
        if (node != (late_insertion ? returned : submit_site)) return;
        expanded = runtime::expand_write(plan, 30, positions, future);
        future.depend(expanded.host_return, late_insertion ? next : suffix);
    });
    require(graph.node(next).completion_time == 21, "total CPU endpoint alone cannot distinguish these insertion sites");
    require(graph.node(returned).completion_time == (late_insertion ? 15 : 20),
            "return must include host submission work, not report completion before the write is issued");
    const uint64_t submitted = late_insertion ? 18 : 13;
    require(graph.node(expanded.completion).completion_time == std::max(busy_us, submitted) + 30,
            "copy respects both submission and the existing write-stream frontier");
    require(graph.node(returned).completion_time < graph.node(expanded.completion).completion_time,
            "asynchronous write-through must not impose a device completion barrier on lifecycle return");
    require_static_replay(graph, "lifecycle insertion must produce a statically replayable graph");
}
void generated_load_layers_need_no_template(size_t count) {
    core::DagGraph graph;
    const auto entry = graph.add_synthetic_node({.name = "submit", .lane_key = "main", .duration = 2});
    const auto cpu_next = graph.add_synthetic_node({.name = "CPU continues", .lane_key = "main", .duration = 1});
    graph.add_edge(entry, cpu_next, core::DagEdgeKind::Sequential);
    const auto ready = graph.add_synthetic_node({.name = "prior H2D", .is_cpu = false, .lane_key = "H2D", .duration = 40});
    (void)graph.add_synthetic_node({.name = "unrelated GPU", .is_cpu = false, .lane_key = "other", .duration = 1000});
    std::vector<size_t> consumers, records;
    for (size_t i = 0; i < count; ++i) {
        const auto consumer = graph.add_synthetic_node({.name = "layer consumer", .is_cpu = false,
            .lane_key = "consumer " + std::to_string(i), .duration = 7, .counts_toward_e2e = true});
        graph.add_edge(entry, consumer, core::DagEdgeKind::Sequential);
        consumers.push_back(consumer);
    }
    const std::vector<uint64_t> bytes(count, 128);
    (void)simulation::run_topological_simulation(graph, {}, [&](size_t node, uint64_t, simulation::FutureDag & future) {
        if (node != entry) return;
        records = runtime::generate_load_layer_transfers(future, ready, "H2D", bytes, 10 * count + 1);
        for (size_t i = 0; i < count; ++i) future.depend(records[i], consumers[i]);
    });
    require(records.size() == count && graph.node(cpu_next).completion_time == 3,
            "new target layers need no source records and do not block CPU return");
    require(graph.node(records.back()).completion_time == 40 + 10 * count + 1,
            "generated load conserves full service, including rounding, and waits for its resource");
    if (count > 1)
        require(graph.node(consumers.front()).completion_time < graph.node(records.back()).completion_time,
                "first layer can compute before the complete batch finishes");
    require_static_replay(graph, "new layer completion nodes remain exact under static DAG replay");
}

void load_index_resources_use_ordinary_submissions() {
    core::DagGraph graph;
    const auto main = graph.add_synthetic_node({.name = "ordinary compute", .lane_key = "main"});
    const auto submit = graph.add_synthetic_node({.name = "Enqueue@aclnnArange", .lane_key = "main"});
    const auto worker = graph.add_synthetic_node({.name = "worker", .lane_key = "worker"});
    const auto device = graph.add_synthetic_node({.name = "Arange", .is_cpu = false, .lane_key = "compute"});
    const auto other_main = graph.add_synthetic_node({.name = "other thread", .lane_key = "other main"});
    const auto submission_edge = graph.add_edge(submit, worker, core::DagEdgeKind::Correlation);
    graph.add_edge(worker, device, core::DagEdgeKind::Correlation);
    simulation::detail::CpuTaskQueues queues;
    queues.tasks.push_back({.first = worker, .last = worker, .submission = submit, .queue = 0});
    queues.node_task.assign(graph.nodes().size(), core::DagNode::kNoNode);
    queues.node_task[worker] = 0;
    const auto observe = [&](size_t node) {
        return runtime::observe_load_index_resources(patch::HiCacheSourceDagIndex(graph), queues, node, device);
    };
    const auto found = observe(main);
    require(found && found->worker == worker && found->compute == device,
            "ordinary base submissions identify resources without a HiCache load template");
    require(!observe(other_main), "another main thread cannot supply load resources");
    graph.mutable_node(submit).active = false;
    graph.mutable_edge(submission_edge).active = false;
    require(!observe(main), "removed submissions cannot witness a resource");
    graph.mutable_node(submit).active = true;
    graph.mutable_edge(submission_edge).active = true;
    const auto other_device = graph.add_synthetic_node({.name = "other stream", .is_cpu = false, .lane_key = "other compute"});
    graph.add_edge(worker, other_device, core::DagEdgeKind::Correlation);
    require(observe(main)->compute == device,
            "the explicit compute role is preserved when the same operation also uses another stream");
    const auto other_worker = graph.add_synthetic_node({.name = "other worker", .lane_key = "other worker"});
    graph.add_edge(submit, other_worker, core::DagEdgeKind::Correlation);
    graph.add_edge(other_worker, device, core::DagEdgeKind::Correlation);
    queues.tasks.push_back({.first = other_worker, .last = other_worker, .submission = submit, .queue = 1});
    queues.node_task.resize(graph.nodes().size(), core::DagNode::kNoNode);
    queues.node_task[other_worker] = 1;
    hicache_timing_fixture::require_throws<std::runtime_error>([&] { (void)observe(main); },
                                                               "conflicting worker FIFOs for the same compute role cannot be silently merged");
}

void generated_ascend_load_has_all_layer_transfer(size_t pages) {
    core::DagGraph graph;
    const auto root = graph.add_synthetic_node({.name = "root", .lane_key = "main"});
    const auto entry = graph.add_synthetic_node({.name = "entry", .lane_key = "main", .duration = 2});
    const auto next = graph.add_synthetic_node({.name = "continue", .lane_key = "main", .duration = 1, .counts_toward_e2e = true});
    const auto worker = graph.add_synthetic_node({.name = "worker", .lane_key = "worker", .duration = 3, .cpu_task_ready_delay_us = 0});
    const auto compute = graph.add_synthetic_node({.name = "prior indices", .is_cpu = false, .lane_key = "compute", .duration = 40});
    const auto load = graph.add_synthetic_node({.name = "prior load", .is_cpu = false, .lane_key = "load", .duration = 20});
    graph.add_edge(root, worker, core::DagEdgeKind::Correlation);
    graph.add_edge(root, entry, core::DagEdgeKind::Sequential);
    graph.add_edge(entry, next, core::DagEdgeKind::Sequential);
    runtime::LoadSubmissionCost cost;
    cost.before_sync.main_us = 2;
    cost.start_record = cost.wait_event = cost.first_layer_record = cost.layer_record = {2, 1, 1, 1, 0};
    cost.first_copy = cost.copy = {1, 1, 0, 0, 0};
    cost.tail.main_us = 2;
    const auto generated = runtime::generate_load_submission(graph, entry, worker, compute, load, pages, 128, 64, cost);
    require(generated.plan.payload.size() == 2 * pages && generated.plan.payload_bytes == pages * 128
            && generated.layer_records.size() == 64, "target pages determine K/V copies; model layers determine records");
    runtime::HiCacheExpandedWrite expanded;
    (void)simulation::run_topological_simulation(graph, {}, [&](size_t node, uint64_t, simulation::FutureDag & future) {
        if (node != entry) return;
        const std::vector<runtime::HiCacheWriteStreamPosition> positions{{compute, {}}, {load, {}}, {compute, {}}};
        expanded = runtime::expand_write(generated.plan, 1000, positions, future, entry);
        future.depend(expanded.host_return, next);
    });
    const auto first_record = expanded.nodes.at(generated.layer_records.front());
    require(graph.node(first_record).completion_time >= 1040, "Ascend first layer waits for all-layer payload and prior compute");
    require(graph.node(next).completion_time < graph.node(first_record).completion_time,
            "CPU returns asynchronously without waiting for the transferred KV");
    for (const auto record : generated.layer_records)
        require(graph.node(expanded.nodes.at(record)).completion_time >= graph.node(first_record).completion_time,
                "no later layer can precede the all-layer transfer");
    require_static_replay(graph, "generated load submission preserves static replay timings");
}

void inactive_layer_positions_need_no_active_sample() {
    core::DagGraph graph;
    const auto add = [&](const char * name, const char * lane, uint64_t at, bool cpu) {
        return graph.add_synthetic_node({.name = name, .is_cpu = cpu, .lane_key = lane, .duration = 1,
            .observed_point = core::DagObservedPoint{"rank", lane, at, 0}});
    };
    const auto before = add("before call", "main", 90, true);
    const auto prior_submit = add("Enqueue@previous", "main", 50, true);
    const auto submit = add("Enqueue@attention", "main", 120, true);
    const auto prior = add("previous", "compute", 80, false);
    const auto attention = add("attention", "compute", 300, false);
    graph.add_edge(prior_submit, prior, core::DagEdgeKind::Correlation);
    graph.add_edge(submit, attention, core::DagEdgeKind::Correlation);
    graph.add_edge(prior, attention, core::DagEdgeKind::Stream);
    HiCacheLayerWaitObservation waits;
    for (const auto [layer, start] : std::vector<std::pair<uint64_t, uint64_t>>{{0, 100}, {0, 105}, {1, 200}}) {
        HiCacheLayerWaitCall call;
        call.phase = "EXTEND"; call.logical_input = 0; call.layer = layer;
        call.batch_start_ns = 80000; call.start_ns = start * 1000; call.end_ns = (start + 2) * 1000;
        call.before = before; call.after = submit;
        waits.calls.push_back(call);
    }
    const std::vector<const HiCacheLayerWaitCall *> calls{&waits.calls[0], &waits.calls[1]};
    const auto observe = [&] { return patch::observe_inactive_layer_positions(patch::HiCacheSourceDagIndex(graph), waits, calls); };
    const auto positions = observe();
    for (const auto * call : calls)
        require(positions.at(call).issue.empty() && positions.at(call).after == attention
                && positions.at(call).before == std::vector<size_t>{prior},
                "two inactive calls in one layer use ordinary CPU submission evidence, not GPU timestamps or an active donor");
    const auto other = add("other attention stream", "other compute", 300, false);
    graph.add_edge(submit, other, core::DagEdgeKind::Correlation);
    require(observe().at(calls[0]).issue == "forward_submission_has_multiple_streams",
            "ambiguous consumer streams are not selected by node order");
}

void generated_layer_wait_uses_target_event() {
    core::DagGraph graph;
    const auto root = graph.add_synthetic_node({.name = "root", .lane_key = "main"});
    const auto entry = graph.add_synthetic_node({.name = "entry", .lane_key = "main", .duration = 2});
    const auto next = graph.add_synthetic_node({.name = "next CPU", .lane_key = "main", .duration = 1, .counts_toward_e2e = true});
    const auto worker = graph.add_synthetic_node({.name = "worker", .lane_key = "worker", .duration = 20, .cpu_task_ready_delay_us = 0});
    const auto compute = graph.add_synthetic_node({.name = "prior compute", .is_cpu = false, .lane_key = "compute", .duration = 40});
    const auto consumer = graph.add_synthetic_node({.name = "attention", .is_cpu = false, .lane_key = "compute", .duration = 1});
    const auto target = graph.add_synthetic_node({.name = "target KV completion", .is_cpu = false, .lane_key = "load", .duration = 1000});
    graph.add_edge(root, entry, core::DagEdgeKind::Sequential);
    graph.add_edge(root, worker, core::DagEdgeKind::Correlation);
    graph.add_edge(entry, next, core::DagEdgeKind::Sequential);
    graph.add_edge(compute, consumer, core::DagEdgeKind::Stream);
    (void)simulation::run_topological_simulation(graph, {}, [&](size_t node, uint64_t, simulation::FutureDag & future) {
        if (node != entry) return;
        const auto plan = runtime::generate_layer_wait(graph, entry, worker, compute, {4, 3, 6, 1, 0}, 5, 2);
        const std::vector<runtime::HiCacheWriteStreamPosition> positions{{compute, consumer}};
        const auto result = runtime::expand_host(plan, positions, future, entry, std::span(&target, 1));
        future.depend(result.host_return, next);
    });
    require(graph.node(next).completion_time == 17, "layer wait returns after CPU submission, not target device completion");
    require(graph.node(consumer).completion_time == 1001, "attention waits for the generated target layer completion");
    require_static_replay(graph, "generated layer wait has exact static replay");
}

void generated_load_index_preserves_async_resources() {
    core::DagGraph graph;
    const auto root = graph.add_synthetic_node({.name = "root", .lane_key = "main"});
    const auto entry = graph.add_synthetic_node({.name = "entry", .lane_key = "main", .duration = 2});
    const auto next = graph.add_synthetic_node({.name = "CPU continues", .lane_key = "main", .duration = 1, .counts_toward_e2e = true});
    const auto worker = graph.add_synthetic_node({.name = "busy worker", .lane_key = "worker", .duration = 20,
        .cpu_task_ready_delay_us = 0});
    const auto compute = graph.add_synthetic_node({.name = "prior compute", .is_cpu = false, .lane_key = "compute", .duration = 40});
    graph.add_edge(root, entry, core::DagEdgeKind::Sequential);
    graph.add_edge(entry, next, core::DagEdgeKind::Sequential);
    graph.add_edge(root, worker, core::DagEdgeKind::Correlation);
    runtime::HiCacheExpandedHost generated;
    (void)simulation::run_topological_simulation(graph, {}, [&](size_t node, uint64_t, simulation::FutureDag & future) {
        if (node != entry) return;
        const auto plan = runtime::generate_load_index_operation(graph, entry, worker, compute, {4, 3, 6, 1, 5}, "range");
        const std::vector<runtime::HiCacheWriteStreamPosition> positions{{compute, std::nullopt}};
        generated = runtime::expand_host(plan, positions, future, entry);
        future.depend(generated.host_return, next);
    });
    require(graph.node(next).completion_time == 10, "index submission returns without waiting for the worker or device");
    require(graph.node(generated.nodes[2]).simulation_start == 21 && graph.node(generated.nodes[3]).completion_time == 45,
            "index operation honors worker FIFO and existing compute-stream work independently");
    require_static_replay(graph, "generated index submission is a statically replayable DAG");
}

void generated_load_preserves_unweighted_records() {
    core::DagGraph graph;
    const auto entry = graph.add_synthetic_node({.name = "submit", .lane_key = "main", .duration = 2});
    const auto ready = graph.add_synthetic_node({.name = "prior H2D", .is_cpu = false, .lane_key = "H2D", .duration = 10});
    const auto late_submit = graph.add_synthetic_node({.name = "record submission", .lane_key = "worker", .duration = 50});
    std::vector<size_t> records;
    for (size_t i = 0; i < 4; ++i) {
        const auto record = graph.add_synthetic_node({.name = "existing record", .is_cpu = false,
            .lane_key = "H2D", .counts_toward_e2e = true});
        graph.add_edge(entry, record, core::DagEdgeKind::Sequential);
        records.push_back(record);
    }
    graph.add_edge(late_submit, records.front(), core::DagEdgeKind::Correlation);
    const std::vector<uint64_t> weights{0, 2, 0, 1};
    (void)simulation::run_topological_simulation(graph, {}, [&](size_t node, uint64_t, simulation::FutureDag & future) {
        if (node != entry) return;
        require(runtime::generate_load_layer_transfers(future, ready, "H2D", weights, 9, records) == records,
                "existing records retain identity even without assigned payload bytes");
    });
    const std::vector<uint64_t> expected{50, 56, 56, 59};
    for (size_t i = 0; i < records.size(); ++i)
        require(graph.node(records[i]).completion_time == expected[i],
                "zero-weight records retain submission dependencies and ordered service totals");
    require_static_replay(graph, "weighted load generation remains statically replayable");
}

void generated_waits_follow_target_completions(size_t count, bool repeat_single_batch = false) {
    core::DagGraph graph;
    const auto entry = graph.add_synthetic_node({.name = "entry", .lane_key = "main", .duration = 2});
    const auto next = graph.add_synthetic_node({.name = "continuation", .lane_key = "main", .duration = 1, .counts_toward_e2e = true});
    graph.add_edge(entry, next, core::DagEdgeKind::Sequential);
    std::vector<size_t> completions;
    const std::vector<uint64_t> times{40, 10, 90};
    for (size_t i = 0; i < count; ++i)
        completions.push_back(graph.add_synthetic_node({.name = "target write", .is_cpu = false,
            .lane_key = "write " + std::to_string(i), .duration = times[i]}));
    const auto unrelated = graph.add_synthetic_node({.name = "unrelated compute", .is_cpu = false, .lane_key = "compute", .duration = 200});
    std::pair<double, double> remainder{};
    (void)simulation::run_topological_simulation(graph, {}, [&](size_t node, uint64_t, simulation::FutureDag & future) {
        if (node != entry) return;
        if (repeat_single_batch) {
            const auto empty = runtime::generated_cpu_control("main", 3, 0, remainder, "empty ACK");
            const auto single = runtime::generated_completion_check("main", 1, 3, 0, 2, 1, remainder);
            const auto last = runtime::expand_write_confirmation_batches(empty, single, completions, entry, future);
            future.depend(last, next);
            return;
        }
        const auto plan = runtime::generated_completion_check("main", count, 3, 0, 2, 1, remainder);
        require(plan.event_waits.size() == count && plan.streams.empty() && plan.waits.empty(),
                "target wait count is generated without a source template or device-wide barrier");
        const auto expanded = runtime::expand_host(plan, {}, future, entry, completions);
        future.depend(expanded.host_return, next);
    });
    uint64_t expected = repeat_single_batch && count ? 2 : 5;
    for (size_t i = 0; i < count; ++i) expected = std::max(expected + (repeat_single_batch ? 3 : 0), times[i]) + 3;
    require(graph.node(next).completion_time == expected + 1 && graph.node(unrelated).completion_time == 200,
            "acknowledgements follow target event order without waiting for unrelated work");
    require_static_replay(graph, "generated target waits must remain exact under static DAG replay");
}

void generated_control_requires_no_source_template() {
    core::DagGraph graph;
    const auto entry = graph.add_synthetic_node({.name = "entry", .lane_key = "main", .duration = 2});
    const auto next = graph.add_synthetic_node({.name = "continuation", .lane_key = "main", .duration = 1, .counts_toward_e2e = true});
    graph.add_edge(entry, next, core::DagEdgeKind::Sequential);
    std::pair<double, double> remainder{};
    (void)simulation::run_topological_simulation(graph, {}, [&](size_t node, uint64_t, simulation::FutureDag & future) {
        if (node != entry) return;
        size_t previous = entry;
        for (int i = 0; i < 4; ++i) {
            const auto plan = runtime::generated_cpu_control("main", 3.25, 1.5, remainder, "target control");
            require(plan.streams.empty() && plan.waits.empty() && plan.event_waits.empty(), "pure generated control cannot invent device dependencies");
            previous = runtime::expand_host(plan, {}, future, previous).host_return;
        }
        future.depend(previous, next);
    });
    require(remainder.first == 0 && remainder.second == 0 && graph.node(next).completion_time == 22,
            "four generated controls retain 13us CPU plus 6us residual before continuation");
    require_static_replay(graph, "generated control is a real replayable DAG, not an endpoint adjustment");
    hicache_timing_fixture::require_throws<std::invalid_argument>([&] { (void)runtime::generated_cpu_control("main", -1, 2, remainder, "invalid"); },
                                                                  "invalid generated control costs cannot be masked by positive residual time");
}

void calibrated_empty_check_preserves_cpu_order() {
    core::DagGraph graph;
    const auto entry = graph.add_synthetic_node({ .name = "capacity entry", .lane_key = "main", .duration = 2 });
    const auto next = graph.add_synthetic_node({ .name = "allocation retry", .lane_key = "main", .duration = 1, .counts_toward_e2e = true });
    const auto busy = graph.add_synthetic_node({ .name = "unrelated device work", .is_cpu = false, .lane_key = "device", .duration = 100 });
    graph.add_edge(entry, next, core::DagEdgeKind::Sequential);
    const std::map<size_t, runtime::HiCacheWriteStreamPosition> positions;
    runtime::HiCacheWriteStreamInsertions insertions;
    double remainder = 0;
    (void)simulation::run_topological_simulation(graph, {}, [&](size_t node, uint64_t, simulation::FutureDag & future) {
        if (node != entry) return;
        runtime::HiCacheHostSequence sequence(positions, insertions, future);
        size_t last = entry;
        for (int i = 0; i < 4; ++i) {
            const auto plan = runtime::calibrated_cpu_control("main", 3.25, remainder, "empty write completion check");
            require(plan.streams.empty() && plan.waits.empty() && plan.event_waits.empty(), "empty check must have no device dependency");
            last = sequence.append(plan, std::span<const size_t>{}).host_return;
        }
        future.depend(last, next);
    });
    require(remainder == 0 && graph.node(next).completion_time == 16, "four checks must total 13us after the original CPU entry");
    require(graph.node(busy).completion_time == 100, "empty checks must not alter unrelated device work");
    double other_thread = 0;
    (void)runtime::calibrated_cpu_control("other", .25, other_thread, "locked eviction candidate");
    require(other_thread == .25 && remainder == 0, "thread-local fractional costs must remain separate");
    uint64_t skipped_cpu = 0;
    for (int i = 0; i < 3; ++i) {
        const auto skip = runtime::calibrated_cpu_control("other", .25, other_thread, "locked eviction candidate");
        skipped_cpu += skip.nodes.front().work.duration;
        require(skip.streams.empty() && skip.waits.empty() && skip.event_waits.empty(), "lock skip adds no device wait or release");
    }
    require(skipped_cpu == 1 && other_thread == 0, "four quarter-microsecond skips accumulate to one microsecond rather than disappearing");
    require_static_replay(graph, "calibrated CPU calls must preserve static replay");
}
void load_expansion_preserves_layer_readiness() {
    core::DagGraph graph;
    const auto entry = graph.add_synthetic_node({ .name = "empty load call", .lane_key = "main" });
    const auto compute = graph.add_synthetic_node({ .name = "prior compute", .is_cpu = false, .lane_key = "compute", .duration = 20 });
    const auto load = graph.add_synthetic_node({ .name = "prior load", .is_cpu = false, .lane_key = "load", .duration = 7 });
    const auto next = graph.add_synthetic_node({ .name = "next CPU call", .lane_key = "main", .duration = 1 });
    const auto consumer = graph.add_synthetic_node({ .name = "layer compute", .is_cpu = false, .lane_key = "compute", .duration = 1, .counts_toward_e2e = true });
    graph.add_edge(entry, next, core::DagEdgeKind::Sequential);
    graph.add_edge(compute, consumer, core::DagEdgeKind::Stream);
    graph.add_edge(next, consumer, core::DagEdgeKind::Correlation);
    runtime::HiCacheWriteExpansion plan;
    plan.nodes = {
        {{ .name = "stream sync self", .lane_key = "main", .duration = 2 }},
        {{ .name = "load submit", .lane_key = "main", .duration = 3 }},
        {{ .name = "load return", .lane_key = "main" }},
        {{ .name = "staging copy", .is_cpu = false, .lane_key = "compute", .duration = 1 }},
        {{ .name = "H2D", .is_cpu = false, .lane_key = "load" }},
        {{ .name = "layer 0 Record", .is_cpu = false, .lane_key = "load" }},
        {{ .name = "layer 1 Record", .is_cpu = false, .lane_key = "load" }}
    };
    plan.edges = {{0, 1, core::DagEdgeKind::Sequential}, {1, 2, core::DagEdgeKind::Sequential},
                  {1, 3, core::DagEdgeKind::Mutation}, {3, 4, core::DagEdgeKind::Sync},
                  {4, 5, core::DagEdgeKind::Stream}, {5, 6, core::DagEdgeKind::Stream}};
    plan.streams = {{compute, 3, 3}, {load, 4, 6}};
    plan.waits = {{compute, 0}};
    plan.host_return = 2;
    plan.write_start = 4;
    plan.completion = 6;
    plan.payload = {{4, 8}};
    plan.payload_bytes = 8;
    const std::vector<runtime::HiCacheWriteStreamPosition> positions{{compute, consumer}, {load, {}}, {compute, {}}};
    runtime::HiCacheExpandedWrite expanded{};
    (void)simulation::run_topological_simulation(graph, {}, [&](size_t node, uint64_t, simulation::FutureDag & future) {
        if (node != entry) return;
        expanded = runtime::expand_write(plan, 11, positions, future);
        future.depend(expanded.host_return, next);
        future.depend(expanded.nodes.at(5), consumer);
        future.depend(expanded.nodes.at(6), consumer);
    });
    require(graph.node(expanded.host_return).completion_time == 25 && graph.node(next).completion_time == 26,
            "load submission returns without waiting for all H2D data");
    require(graph.node(expanded.nodes.at(5)).completion_time == 37 && graph.node(expanded.nodes.at(6)).completion_time == 37,
            "all-layer DMA before the first Record must not be divided among readiness markers");
    require(graph.node(consumer).completion_time == 38, "layer computation must await predicted data and original stream work");
    require_static_replay(graph, "new load host, streams and layer dependencies survive static replay");
}
void failed_load_precedes_successful_retry(bool needs_capacity, uint64_t preparation_us = 0) {
    core::DagGraph graph;
    const auto entry = graph.add_synthetic_node({ .name = "load allocation entry", .lane_key = "main" });
    const auto success = graph.add_synthetic_node({ .name = "successful allocation", .lane_key = "main", .duration = 5, .cpu_gap_after = 7 });
    const auto submit = graph.add_synthetic_node({ .name = "load submission", .lane_key = "main", .duration = 2 });
    const auto device = graph.add_synthetic_node({ .name = "load DMA", .is_cpu = false, .lane_key = "load", .duration = 3 });
    const auto written = graph.add_synthetic_node({ .name = "write completion", .is_cpu = false, .lane_key = "write", .duration = 40 });
    graph.mutable_node(device).counts_toward_e2e = true;
    graph.add_edge(entry, success, core::DagEdgeKind::Sequential);
    graph.add_edge(success, submit, core::DagEdgeKind::Sequential);
    graph.add_edge(submit, device, core::DagEdgeKind::Correlation);
    runtime::HiCacheHostExpansion preparation, failed, check, release;
    preparation.nodes.push_back({ { .name = "load admission preparation", .lane_key = "main", .duration = preparation_us } });
    failed.nodes.push_back({ { .name = "failed allocation", .lane_key = "main", .duration = 13 } });
    check.nodes.push_back({ { .name = "write confirmation", .lane_key = "main", .duration = 2 } });
    check.event_waits.push_back({ written, 0 });
    release.nodes.push_back({ { .name = "device capacity release", .lane_key = "main", .duration = 3 } });
    runtime::HiCacheWriteStreamInsertions insertions;
    const std::map<size_t, runtime::HiCacheWriteStreamPosition> positions;
    const auto result = simulation::run_topological_simulation(graph, {}, [&](size_t node, uint64_t, simulation::FutureDag & future) {
        if (node != entry || !needs_capacity) return;
        runtime::HiCacheHostSequence before(positions, insertions, future);
        const auto prepared = before.append(preparation, std::span<const size_t>{});
        runtime::HiCacheHostSequence sequence(positions, insertions, future, prepared.host_return);
        (void)sequence.append(failed, std::span<const size_t>{});
        const std::vector<size_t> completions{ written };
        (void)sequence.append(check, std::span<const size_t>{}, completions);
        const auto freed = sequence.append(release, std::span<const size_t>{});
        future.depend(freed.host_return, success);
    });
    require(result.cpu_task_count == 0, "pure CPU failed attempt must not invent a worker queue");
    const auto retry_start = needs_capacity ? std::max<uint64_t>(preparation_us + 13, 40) + 5 : 0;
    require(graph.node(success).simulation_start == retry_start, "successful retry waits for preparation, confirmation and release even across separate sequences");
    require(graph.node(device).completion_time == retry_start + 17, "original success work and gap remain after capacity release without duplication");
    require_static_replay(graph, "load retry expanded dependencies survive static replay");
}
void event_wait_order_follows_cpu_calls() {
    core::DagGraph graph;
    const auto later_record = graph.add_synthetic_node({ .name = "EVENT_RECORD", .is_cpu = false, .lane_key = "write" });
    const auto later = graph.add_synthetic_node({
        .name = "AscendCL@aclrtSynchronizeEvent",
        .lane_key = "main",
        .duration = 2,
        .observed_point = core::DagObservedPoint{ "p", "main", 200, 0 }
    });
    const auto first_record = graph.add_synthetic_node({ .name = "EVENT_RECORD", .is_cpu = false, .lane_key = "write" });
    const auto first = graph.add_synthetic_node({
        .name = "AscendCL@aclrtSynchronizeEvent",
        .lane_key = "main",
        .duration = 100,
        .observed_point = core::DagObservedPoint{ "p", "main", 100, 0 }
    });
    graph.set_node_duration(first, 2); // CPU self cost replaces observed blocking wall time.
    graph.add_edge(first_record, first, core::DagEdgeKind::Sync);
    graph.add_edge(later_record, later, core::DagEdgeKind::Sync);
    graph.add_edge(first, later, core::DagEdgeKind::Sequential);
    runtime::HiCacheHostTemplate host;
    host.main.owned_node_ids = { later, first };
    const auto plan = runtime::prepare_host_expansion(patch::HiCacheSourceDagIndex(graph), simulation::detail::discover_cpu_task_queues(graph), host);
    require(plan.waits.empty(), "specific event waits are not whole-stream barriers");
    require(plan.event_waits.size() == 2 && plan.event_waits[0].source_node == first_record && plan.event_waits[1].source_node == later_record,
            "the target FIFO must bind event waits in observed CPU call order, not source node ID order");
    const auto decision = graph.add_synthetic_node({ .name = "target confirmation entry", .lane_key = "main" });
    const auto first_done = graph.add_synthetic_node({ .name = "first target completion", .is_cpu = false, .lane_key = "target first", .duration = 40 });
    const auto second_done = graph.add_synthetic_node({ .name = "second target completion", .is_cpu = false, .lane_key = "target second", .duration = 100 });
    const auto next = graph.add_synthetic_node({ .name = "after both confirmations", .lane_key = "main", .duration = 1, .counts_toward_e2e = true });
    graph.add_edge(later, decision, core::DagEdgeKind::Sequential);
    graph.add_edge(decision, next, core::DagEdgeKind::Sequential);
    const std::vector<size_t> completions{ first_done, second_done };
    runtime::HiCacheExpandedHost expanded{};
    (void)simulation::run_topological_simulation(graph, {}, [&](size_t node, uint64_t, simulation::FutureDag & future) {
        if (node != decision) return;
        require_throws<std::invalid_argument>([&] { (void)runtime::expand_host(plan, {}, future); }, "unbound event waits must not retain donor completions");
        expanded = runtime::expand_host(plan, {}, future, std::nullopt, completions);
        future.depend(expanded.host_return, next);
    });
    require(graph.node(expanded.nodes.at(plan.event_waits[0].consumer)).completion_time == 42,
            "first acknowledgement must not wait for the entire target batch");
    require(graph.node(expanded.host_return).completion_time == 102 && graph.node(next).completion_time == 103,
            "second acknowledgement and continuation wait for the second target completion");
    require_static_replay(graph, "two explicit event bindings survive static replay");
}
void gap_only_control_fragment() {
    core::DagGraph graph;
    const auto before = graph.add_synthetic_node({
        .name = "before",
        .lane_key = "main",
        .duration = 2,
        .cpu_gap_after = 74,
        .observed_point = core::DagObservedPoint{ "p", "main", 100, 0 }
    });
    const auto after = graph.add_synthetic_node({
        .name = "after",
        .lane_key = "main",
        .duration = 1,
        .observed_point = core::DagObservedPoint{ "p", "main", 176, 0 }
    });
    graph.add_edge(before, after, core::DagEdgeKind::Sequential);
    graph.mutable_node(after).counts_toward_e2e = true;
    const patch::HiCacheSourceDagIndex source(graph);
    const auto host = runtime::observe_host_template(source, "p", "main", 102, 176);
    require(host.main.owned_node_ids.empty() && host.main.owned_gap_duration_us == 74, "pure observed gap is a valid control fragment, not CPU service");
    const auto plan = runtime::prepare_host_expansion(source, simulation::detail::discover_cpu_task_queues(graph), host);
    require(plan.nodes.size() == 2 && plan.nodes.front().work.duration == 0 && plan.nodes.front().work.cpu_gap_after == 74,
            "gap-only template retains the gap without inventing a CPU leaf");
    runtime::HiCacheExpandedHost expanded{};
    (void)simulation::run_topological_simulation(graph, {}, [&](size_t node, uint64_t, simulation::FutureDag & future) {
        if (node == after) expanded = runtime::expand_host(plan, {}, future);
    });
    require(graph.node(expanded.host_return).completion_time == graph.node(after).completion_time + 74,
            "gap-only fragment remains explicit elapsed time when expanded");
}
void asynchronous_event_wait_uses_target_layer(const char * name, uint64_t target_us, uint64_t host_gap_us = 0) {
    core::DagGraph graph;
    const auto record = graph.add_synthetic_node({ .name = "EVENT_RECORD", .is_cpu = false, .lane_key = "old load", .duration = 1000 });
    const auto submit = graph.add_synthetic_node({ .name = "Enqueue@wait_event", .lane_key = "main", .duration = 2,
                                                   .observed_point = core::DagObservedPoint{ "p", "main", 100, 0 } });
    const auto wait = graph.add_synthetic_node({ .name = name, .is_cpu = false, .lane_key = "compute" });
    graph.add_edge(submit, wait, core::DagEdgeKind::Correlation);
    graph.add_edge(record, wait, core::DagEdgeKind::Sync);
    runtime::HiCacheHostTemplate host;
    host.main.owned_node_ids = { submit };
    if (host_gap_us) host.main.owned_gap_slices.push_back({ .owner_node_id = submit,
        .gap_start_us = 102, .gap_end_us = 102 + host_gap_us,
        .owned_start_us = 102, .owned_end_us = 102 + host_gap_us });
    const auto plan = runtime::prepare_host_expansion(patch::HiCacheSourceDagIndex(graph), simulation::detail::discover_cpu_task_queues(graph), host);
    require(plan.waits.empty() && plan.event_waits.size() == 1 && plan.streams.size() == 1,
            "a layer event is an explicit completion dependency, not a whole-stream barrier");
    const auto decision = graph.add_synthetic_node({ .name = "decision", .lane_key = "new main" });
    const auto target = graph.add_synthetic_node({ .name = "target layer ready", .is_cpu = false, .lane_key = "new load", .duration = target_us });
    const auto next = graph.add_synthetic_node({ .name = "target compute", .is_cpu = false, .lane_key = "new compute", .duration = 3,
                                                 .counts_toward_e2e = true });
    graph.add_edge(decision, next, core::DagEdgeKind::Correlation);
    const runtime::HiCacheWriteStreamPosition position{ std::nullopt, next };
    runtime::HiCacheExpandedHost expanded{};
    (void)simulation::run_topological_simulation(graph, {}, [&](size_t id, uint64_t, simulation::FutureDag & future) {
        if (id == decision) expanded = runtime::expand_host(plan, std::span(&position, 1), future, std::nullopt, std::span(&target, 1));
    });
    require(graph.node(expanded.host_return).completion_time == 2 + host_gap_us,
            "CPU return retains its own gap without adding asynchronous device waiting");
    require(graph.node(next).completion_time == std::max(uint64_t{2}, target_us) + 3,
            "compute waits for its target layer and submission, not the old record");
    require_static_replay(graph, "asynchronous target layer binding survives static replay");
}
void release_without_payload(size_t repetitions) {
    core::DagGraph graph;
    const auto add = [&](const char * name, bool cpu, const char * lane, uint64_t at, uint64_t duration, std::optional<uint64_t> ready = std::nullopt) {
        return graph.add_synthetic_node({
            .name = name,
            .is_cpu = cpu,
            .lane_key = lane,
            .duration = duration,
            .cpu_task_ready_delay_us = ready,
            .observed_point = core::DagObservedPoint{ "p", lane, at, 0 }
        });
    };
    const auto submit = add("enqueue free indices", true, "main", 100, 2);
    const auto bookkeeping = add("radix bookkeeping", true, "main", 102, 3);
    const auto worker = add("free indices worker", true, "worker", 105, 4, 0);
    const auto device = add("free indices device", false, "compute", 130, 5);
    const auto before = add("busy compute", false, "compute", 0, 30);
    const auto decision = add("capacity decision", true, "main", 150, 0);
    const auto after = add("later allocation", false, "compute", 160, 7);
    const auto continuation = add("allocator CPU", true, "main", 160, 1);
    graph.mutable_node(after).counts_toward_e2e = true;
    graph.add_edge(submit, bookkeeping, core::DagEdgeKind::Sequential);
    graph.add_edge(submit, worker, core::DagEdgeKind::Correlation);
    graph.add_edge(worker, device, core::DagEdgeKind::Correlation);
    graph.add_edge(before, device, core::DagEdgeKind::Stream);
    graph.add_edge(bookkeeping, decision, core::DagEdgeKind::Sequential);
    graph.add_edge(device, after, core::DagEdgeKind::Stream);
    graph.add_edge(decision, after, core::DagEdgeKind::Correlation);
    graph.add_edge(decision, continuation, core::DagEdgeKind::Sequential);
    runtime::HiCacheHostTemplate host;
    host.main.owned_node_ids = { submit, bookkeeping };
    host.worker_nodes = { worker };
    const auto plan = runtime::prepare_host_expansion(patch::HiCacheSourceDagIndex(graph), simulation::detail::discover_cpu_task_queues(graph), host);
    require(plan.streams.size() == 1 && plan.source_nodes.contains(device), "release work follows submission through worker to device without any DMA payload");
    runtime::HiCacheExpandedHost expanded{};
    const std::vector<size_t> lanes{ graph.node(device).lane_id };
    const std::map<size_t, runtime::HiCacheWriteStreamPosition> positions{ { lanes.front(), { device, after } } };
    runtime::HiCacheWriteStreamInsertions insertions;
    const auto result = simulation::run_topological_simulation(graph, {}, [&](size_t node, uint64_t, simulation::FutureDag & future) {
        if (node != decision) return;
        runtime::HiCacheHostSequence sequence(positions, insertions, future);
        for (size_t i = 0; i < repetitions; ++i) expanded = sequence.append(plan, lanes);
        future.depend(expanded.host_return, continuation);
    });
    require(result.cpu_task_count == repetitions + 1 && result.cpu_queue_count == 1, "new release work shares the existing runtime queue");
    require(graph.node(after).completion_time == 42 + 5 * repetitions, "each release advances the same device frontier before the later allocation");
    require(graph.node(expanded.host_return).completion_time < graph.node(expanded.stream_tails.front()).completion_time,
            "returning from asynchronous free must not add an invented device synchronization");
    require(graph.node(continuation).simulation_start == graph.node(expanded.host_return).completion_time,
            "allocator continuation waits for new host bookkeeping, not the donor's time");
    require_static_replay(graph, "release control expansion remains executable in static replay");
}
void copied_worker_joins_surviving_queue(int rank) {
    core::DagGraph graph;
    const auto add = [&](const char * name, const char * lane, uint64_t duration, std::optional<uint64_t> ready = std::nullopt) {
        return graph.add_synthetic_node({ .name = name, .lane_key = lane, .duration = duration, .cpu_task_ready_delay_us = ready,
                                          .observed_point = core::DagObservedPoint{ "p", lane, 100, rank } });
    };
    const auto submit = add("old enqueue", "main", 2);
    const auto worker = add("old worker", "worker", 4, 0);
    const auto survivor_submit = add("surviving enqueue", "main", 1);
    const auto survivor = add("surviving worker", "worker", 20, 0);
    const auto orphan = add("unproven worker", "other worker", 0);
    graph.add_edge(submit, worker, core::DagEdgeKind::Correlation);
    graph.add_edge(survivor_submit, survivor, core::DagEdgeKind::Correlation);
    runtime::HiCacheHostTemplate host;
    host.main.owned_node_ids = { submit };
    host.worker_nodes = { worker };
    auto plan = runtime::prepare_host_expansion(patch::HiCacheSourceDagIndex(graph), simulation::detail::discover_cpu_task_queues(graph), host);
    const auto local_worker = plan.source_nodes.at(worker);
    core::DagMutationPlan removal{ .component = "layer_wait_runtime" };
    removal.disable_nodes = { worker };
    (void)core::apply_dag_mutation_plan(graph, removal);
    auto unsupported = plan;
    unsupported.nodes[local_worker].queue_member = orphan;
    std::vector<runtime::HiCacheHostExpansion *> plans{ &plan, &unsupported };
    bool rejected = false;
    try { runtime::rebind_host_worker_queues(graph, plans); } catch (const std::runtime_error &) { rejected = true; }
    require(rejected && plan.nodes[local_worker].queue_member == worker, "missing queue evidence cannot partially rewrite another template");
    plans.resize(1);
    runtime::rebind_host_worker_queues(graph, plans);
    require(plan.nodes[local_worker].queue_member == survivor, "removed worker must rebind to a surviving task on the same lane");
    const auto decision = add("decision", "main", 0);
    const auto next = add("consumer", "main", 1);
    graph.mutable_node(next).counts_toward_e2e = true;
    graph.add_edge(decision, next, core::DagEdgeKind::Sequential);
    runtime::HiCacheExpandedHost expanded{};
    const auto result = simulation::run_topological_simulation(graph, {}, [&](size_t id, uint64_t, simulation::FutureDag & future) {
        if (id != decision) return;
        expanded = runtime::expand_host(plan, {}, future);
        future.depend(expanded.nodes.at(local_worker), next);
    });
    require(result.cpu_queue_count == 1 && result.cpu_task_count == 2, "new task uses the surviving FIFO, not a fabricated queue");
    require(graph.node(expanded.nodes.at(local_worker)).gpu_id == rank, "worker expansion preserves a nonzero caller rank");
    require(graph.node(expanded.host_return).completion_time == 2 && graph.node(next).completion_time == 26,
            "CPU returns asynchronously while the copied worker waits behind the existing twenty-us task");
    require_static_replay(graph, "rebound worker queue timing survives static replay");
}
void insertions_across_callbacks() {
    core::DagGraph graph;
    const auto before = graph.add_synthetic_node({ .name = "old compute", .is_cpu = false, .lane_key = "compute", .duration = 30 });
    const auto first = graph.add_synthetic_node({ .name = "first check", .lane_key = "main" });
    const auto second = graph.add_synthetic_node({ .name = "second check", .lane_key = "main", .duration = 2 });
    const auto after = graph.add_synthetic_node({ .name = "later compute", .is_cpu = false, .lane_key = "compute", .duration = 10 });
    graph.mutable_node(after).counts_toward_e2e = true;
    graph.add_edge(first, second, core::DagEdgeKind::Sequential);
    graph.add_edge(second, after, core::DagEdgeKind::Correlation);
    graph.add_edge(before, after, core::DagEdgeKind::Stream);
    runtime::HiCacheWriteStreamInsertions insertions;
    const runtime::HiCacheWriteStreamPosition source{ before, after };
    const auto lane = graph.node(before).lane_id;
    std::vector<size_t> records;
    (void)simulation::run_topological_simulation(graph, {}, [&](size_t node, uint64_t, simulation::FutureDag & future) {
        if (node != first && node != second) return;
        const auto position = insertions.position(lane, source);
        const auto record = future.append({ .name = "new Record", .is_cpu = false, .lane_key = "compute", .duration = 5 });
        future.depend(*position.before, record);
        future.depend(record, *position.after);
        insertions.advance(lane, source, record);
        records.push_back(record);
    });
    require(graph.node(records[0]).simulation_start == 30 && graph.node(records[1]).simulation_start == 35,
            "insertions from separate callbacks share the current stream tail");
    require(graph.node(after).completion_time == 50, "the native continuation waits for both serialized insertions");
    require(insertions.position(lane, { after, std::nullopt }).before == after, "a later native frontier supersedes the earlier inserted tail");
    require(insertions.position(lane + 1, source).before == before, "stream insertion history cannot cross lanes");
    require_static_replay(graph, "cross-callback stream order must survive static replay");
}
void expand_with_busy_stream(uint64_t compute_us, bool intermediate_record = false, bool second_call = false, bool external_wait = false,
                             bool empty_write_stream = false) {
    core::DagGraph graph;
    const auto add = [&](const char * name, bool cpu, const char * lane, uint64_t at, uint64_t duration, std::optional<uint64_t> ready = std::nullopt) {
        return graph.add_synthetic_node({
            .name = name,
            .is_cpu = cpu,
            .lane_key = lane,
            .duration = duration,
            .cpu_task_ready_delay_us = ready,
            .observed_point = core::DagObservedPoint{ "p", cpu ? lane : "device", at, 0 }
        });
    };
    add("enqueue Record", true, "main", 100, 2);              // 0
    add("submit payload", true, "main", 110, 1);              // 1
    add("Record worker", true, "worker", 105, 4, 0);          // 2
    add("EVENT_RECORD", false, "compute", 150, 1);            // 3
    add("EVENT_WAIT", false, "write", 155, 1);                // 4
    add("D2H first", false, "write", 160, 0);                 // 5
    add("D2H second", false, "write", 170, 0);                // 6
    add("EVENT_RECORD", false, "write", 180, 1);              // 7
    add("earlier compute", false, "compute", 90, compute_us); // 8
    const auto decision = add("new write decision", true, "main", 200, 0);
    const auto after = add("later compute", false, "compute", 300, 7);
    const auto consumer = add("after write", true, "main", 400, 3);
    graph.mutable_node(consumer).counts_toward_e2e = true;
    graph.mutable_node(0).cpu_gap_after = graph.mutable_node(0).original_cpu_gap_after = 8;
    graph.add_edge(0, 1, core::DagEdgeKind::Sequential);
    graph.add_edge(0, 2, core::DagEdgeKind::Correlation);
    graph.add_edge(2, 3, core::DagEdgeKind::Correlation);
    graph.add_edge(0, 4, core::DagEdgeKind::Correlation);
    for (const auto id : { 5, 6, 7 }) graph.add_edge(1, id, core::DagEdgeKind::Correlation);
    graph.add_edge(8, 3, core::DagEdgeKind::Stream);
    graph.add_edge(3, 4, core::DagEdgeKind::Sync);
    graph.add_edge(4, 5, core::DagEdgeKind::Stream);
    graph.add_edge(6, 7, core::DagEdgeKind::Stream);
    if (intermediate_record) {
        const auto marker = add("EVENT_RECORD", false, "write", 165, 1);
        graph.add_edge(1, marker, core::DagEdgeKind::Correlation);
        graph.add_edge(5, marker, core::DagEdgeKind::Stream);
        graph.add_edge(marker, 6, core::DagEdgeKind::Stream);
    }
    else graph.add_edge(5, 6, core::DagEdgeKind::Stream);
    graph.add_edge(1, decision, core::DagEdgeKind::Sequential);
    graph.add_edge(decision, after, core::DagEdgeKind::Correlation);
    graph.add_edge(3, after, core::DagEdgeKind::Stream);
    graph.add_edge(decision, consumer, core::DagEdgeKind::Mutation);
    for (const auto id : { 5, 6 }) graph.mutable_event_for_node(id).set_arg("size(B)", "16");
    runtime::HiCacheHostTemplate host;
    host.main.owned_node_ids = { 0, 1 };
    host.main.owned_gap_slices.push_back(
        { .owner_node_id = 0, .successor_node_id = 1, .gap_start_us = 102, .gap_end_us = 110, .owned_start_us = 102, .owned_end_us = 110 });
    host.worker_nodes = { 2 };
    if (external_wait) {
        const auto sync = add("AscendCL@aclrtSynchronizeStream", true, "main", 95, 2);
        graph.add_edge(8, sync, core::DagEdgeKind::Sync);
        graph.add_edge(sync, 0, core::DagEdgeKind::Sequential);
        host.main.owned_node_ids.insert(host.main.owned_node_ids.begin(), sync);
    }
    patch::HiCacheIoOperationRecord record;
    record.kind = patch::HiCacheIoOperationKind::WriteDeviceToHost;
    record.device_transfer_node_ids = { 6, 5 }; // Source IDs/list order cannot set stream order.
    const auto queues = simulation::detail::discover_cpu_task_queues(graph);
    auto plan = runtime::prepare_write_expansion(patch::HiCacheSourceDagIndex(graph), queues, record, host, 7);
    require(plan.nodes.at(plan.source_nodes.at(2)).work.duration == 4 && plan.payload_bytes == 32,
            "template cost and transfer geometry have explicit source observations");
    require(plan.nodes.at(plan.write_start).work.name == "target write: EVENT_WAIT", "write serialization starts before its Wait, not at its first DMA");
    std::optional<size_t> empty_resource;
    if (empty_write_stream) {
        empty_resource = add("unused write resource", false, "new write stream", 0, 0);
        std::map<std::string, size_t> resources;
        for (const auto & [original, local] : plan.source_nodes)
            resources[std::string(graph.node_lane_key(original))] = original;
        const auto portable = runtime::export_write_template(graph, plan, resources);
        resources["write"] = *empty_resource;
        plan = runtime::import_write_template(portable, graph, queues, resources);
        graph.mutable_node(*empty_resource).active = false; // Identity, not executable work.
    }
    std::map<size_t, runtime::HiCacheWriteStreamPosition> positions;
    std::vector<size_t> lanes;
    for (const auto & stream : plan.streams) {
        const auto lane = graph.node(stream.source_node).lane_id;
        lanes.push_back(lane);
        positions[lane] = lane == graph.node(3).lane_id ? runtime::HiCacheWriteStreamPosition{ 3, after }
                                                      : runtime::HiCacheWriteStreamPosition{ 7, std::nullopt };
        if (empty_resource && lane == graph.node(*empty_resource).lane_id) {
            positions[lane] = runtime::observe_write_stream_position(patch::HiCacheSourceDagIndex(graph), *empty_resource, "p", "main", 200);
            require(!positions[lane].before && !positions[lane].after, "unused stream has no inherited device endpoints");
        }
    }
    require(plan.waits.size() == (external_wait ? 1 : 0), "external native synchronization is retained as a separate wait");
    for (const auto & wait : plan.waits) {
        const auto lane = graph.node(wait.source_node).lane_id;
        lanes.push_back(lane);
        positions[lane] = { 3, after };
    }
    runtime::HiCacheWriteStreamInsertions insertions;
    runtime::HiCacheExpandedWrite expanded{};
    runtime::HiCacheExpandedWrite second{};
    const auto result = simulation::run_topological_simulation(graph, {}, [&](size_t node, uint64_t, simulation::FutureDag & future) {
        if (node != decision) return;
        runtime::HiCacheHostSequence sequence(positions, insertions, future);
        expanded = sequence.append(plan, 20, lanes);
        future.depend(expanded.completion, consumer);
        if (second_call) {
            second = sequence.append(plan, 20, lanes);
            future.depend(second.completion, consumer);
        }
    });
    uint64_t payload_cost = 0;
    for (const auto & node : graph.nodes())
        if (graph.event_for_node(node.id).name.starts_with("target write: D2H")) payload_cost += node.duration;
    require(payload_cost == (second_call ? 40 : 20), "new DMA costs conserve target service rather than retaining donor payload duration");
    require(result.cpu_task_count == (second_call ? 3 : 2) && result.cpu_queue_count == 1, "new Record task shares the original worker");
    require(graph.node(expanded.host_return).completion_time < graph.node(expanded.completion).completion_time,
            "host return must not become a full device-service barrier");
    for (const auto & [local, bytes] : plan.payload) {
        const auto & transfer = graph.node(expanded.nodes.at(local));
        if (!empty_write_stream)
            require(transfer.simulation_start >= graph.node(7).completion_time, "new payload cannot overtake prior write-stream work");

        require(transfer.simulation_start >= graph.node(3).completion_time,
                "even an empty write stream must wait for compute readiness");
    }
    require(graph.node(after).simulation_start >= graph.node(3).completion_time, "the compute continuation cannot precede its original data frontier");
    for (const auto & node : graph.nodes())
        if (node.lane_id == graph.node(3).lane_id && graph.event_for_node(node.id).name == "target write: EVENT_RECORD")
            require(graph.node(after).simulation_start >= node.completion_time, "later compute must also wait for the newly inserted Record");
    require(graph.node(consumer).simulation_start == graph.node(second_call ? second.completion : expanded.completion).completion_time,
            "capacity consumer waits for new transfer completion");
    if (second_call) {
        require(graph.node(second.host_return).completion_time >= graph.node(expanded.host_return).completion_time + 11,
                "consecutive host calls cannot overlap on the main thread");
        for (size_t i = 0; i < plan.streams.size(); ++i)
            require(graph.node(second.stream_tails[i]).completion_time > graph.node(expanded.stream_tails[i]).completion_time,
                    "each new call advances every device stream, including compute Records");
        require(graph.node(second.write_start).simulation_start >= graph.node(expanded.completion).completion_time,
                "the second write stream begins after the first completion Record");
    }
    size_t syncs = 0;
    for (const auto & node : graph.nodes()) {
        if (graph.event_for_node(node.id).name != "target write: AscendCL@aclrtSynchronizeStream") continue;
        require(node.duration == 2, "native synchronization retains only its CPU service, not donor wait time");
        size_t expected = 3;
        if (syncs++)
            for (size_t i = 0; i < plan.streams.size(); ++i)
                if (graph.node(plan.streams[i].source_node).lane_id == graph.node(3).lane_id) expected = expanded.stream_tails[i];
        bool rebound = false;
        for (const auto & edge : graph.edges()) {
            if (!edge.active || edge.dst != node.id) continue;
            require(edge.src != 8, "new synchronization must not copy the donor's obsolete device frontier");
            rebound |= edge.src == expected;
        }
        require(rebound, "native synchronization waits at this call's current stream frontier");
    }
    require(syncs == (external_wait ? (second_call ? 2 : 1) : 0), "each expanded call retains its native synchronization");
    require_static_replay(graph, "expanded host, queue and GPU work must replay identically");
}
} // namespace

void check_write_expansion() {
    host_and_worker_use_only_measured_cpu_service();
    for (const auto count : {0u, 1u, 2u, 3u}) generated_waits_follow_target_completions(count);
    for (const auto count : {0u, 1u, 2u, 3u}) generated_waits_follow_target_completions(count, true);
    generated_control_requires_no_source_template();
    independent_host_resources_are_rebound();
    independent_host_resources_are_rebound(true);
    for (const auto busy_us : {uint64_t{0}, uint64_t{40}}) {
        lifecycle_write_precedes_return(false, busy_us);
        lifecycle_write_precedes_return(true, busy_us); // Same E2E, wrong lifecycle boundary.
    }
    calibrated_empty_check_preserves_cpu_order();
    load_expansion_preserves_layer_readiness();
    generated_load_layers_need_no_template(1);
    generated_load_layers_need_no_template(3);
    generated_load_layers_need_no_template(48);
    generated_load_index_preserves_async_resources();
    generated_layer_wait_uses_target_event();
    inactive_layer_positions_need_no_active_sample();
    generated_ascend_load_has_all_layer_transfer(1);
    generated_ascend_load_has_all_layer_transfer(22);
    load_index_resources_use_ordinary_submissions();
    generated_load_preserves_unweighted_records();
    failed_load_precedes_successful_retry(false);
    failed_load_precedes_successful_retry(true);
    failed_load_precedes_successful_retry(true, 80);
    event_wait_order_follows_cpu_calls();
    gap_only_control_fragment();
    asynchronous_event_wait_uses_target_layer("EVENT_WAIT", 1);
    asynchronous_event_wait_uses_target_layer("EVENT_WAIT", 40);
    asynchronous_event_wait_uses_target_layer("logical_event_wait", 1);
    asynchronous_event_wait_uses_target_layer("logical_event_wait", 40);
    asynchronous_event_wait_uses_target_layer("EVENT_WAIT", 1, 17);
    asynchronous_event_wait_uses_target_layer("EVENT_WAIT", 40, 17);
    release_without_payload(1);
    release_without_payload(2);
    copied_worker_joins_surviving_queue(0);
    copied_worker_joins_surviving_queue(2);
    insertions_across_callbacks();
    expand_with_busy_stream(0);
    expand_with_busy_stream(40);
    expand_with_busy_stream(40, true);
    expand_with_busy_stream(0, false, true);
    expand_with_busy_stream(40, true, true);
    expand_with_busy_stream(40, false, false, true);
    expand_with_busy_stream(40, true, true, true);
    expand_with_busy_stream(40, false, true, false, true);
}
