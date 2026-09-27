#include "hicache_timing_fixture.hpp"
#include "markov/trace_graph/modules/hicache/runtime/writes.hpp"
#include <algorithm>
#include <stdexcept>

using namespace hicache_timing_fixture;
namespace {
struct WriteGraph {
    core::DagGraph graph;
    patch::HiCacheIoOperationRecord record;
};
WriteGraph observed_write(uint64_t compute_us, uint64_t later_submit_us) {
    // CPU: submit ---- later submit       CPU can continue without waiting.
    // GPU: compute -> Record -> Wait -> DMA(1 byte) -> DMA(2 bytes) -> Record
    // Both DMA submissions and the source data are independent prerequisites.
    WriteGraph result;
    const auto add = [&](const char * name, bool cpu, const char * lane, uint64_t duration) {
        return result.graph.add_synthetic_node({ .name = name, .is_cpu = cpu, .lane_key = lane, .duration = duration });
    };
    add("submit", true, "main", 5);                     // 0
    add("compute", false, "compute", compute_us);       // 1
    add("EVENT_RECORD", false, "compute", 0);           // 2
    add("EVENT_WAIT", false, "write", 0);               // 3
    add("D2H first", false, "write", 99);               // 4
    add("later submit", true, "main", later_submit_us); // 5
    add("D2H second", false, "write", 99);              // 6
    add("EVENT_RECORD", false, "write", 2);             // 7
    add("CPU independent", true, "main", 3);            // 8
    add("blocking consumer", true, "wait", 1);          // 9
    result.graph.mutable_node(9).counts_toward_e2e = true;
    for (const auto [from, to] : std::vector<std::pair<size_t, size_t>>{
             { 1, 2 },
             { 3, 4 },
             { 4, 6 },
             { 6, 7 }
    })
        result.graph.add_edge(from, to, core::DagEdgeKind::Stream);
    result.graph.add_edge(2, 3, core::DagEdgeKind::Sync);
    for (const auto to : { 3, 4, 5, 8 }) result.graph.add_edge(0, to, core::DagEdgeKind::Mutation);
    result.graph.add_edge(5, 6, core::DagEdgeKind::Mutation);
    result.graph.add_edge(7, 9, core::DagEdgeKind::Sync);
    for (const auto [node, bytes] : std::vector<std::pair<size_t, uint64_t>>{
             { 4, 1 },
             { 6, 2 }
    }) {
        auto & event = result.graph.mutable_events().at(result.graph.node(node).event_index);
        event.set_arg("operation", "device to host");
        event.set_arg("size(B)", std::to_string(bytes));
    }
    result.graph.mutable_node(7).submit_ts = 105;
    result.record.kind = patch::HiCacheIoOperationKind::WriteDeviceToHost;
    result.record.timing_fact_node_id = 42;
    result.record.source_start_us = 100;
    result.record.source_end_us = 160;
    result.record.device_transfer_node_ids = { 4, 6 };
    result.record.device_completion_node_ids = { 7 };
    return result;
}
void redundant_stream_edges_keep_one_order() {
    auto fixture = observed_write(30, 0);
    auto & graph = fixture.graph;
    const auto wait = graph.add_synthetic_node({ .name = "logical_event_wait", .is_cpu = false, .lane_key = "write" });
    graph.add_edge(3, wait, core::DagEdgeKind::Stream);
    const auto onward = graph.add_edge(wait, 4, core::DagEdgeKind::Stream);
    graph.add_edge(2, wait, core::DagEdgeKind::Sync);
    graph.add_edge(3, 4, core::DagEdgeKind::Stream); // Duplicate and transitive edges are both legal.
    graph.mutable_node(2).submit_ts = 105;
    patch::HiCacheSourceDagIndex source(graph);
    const auto & order = source.device_stream_order(4);
    require(order.nodes == std::vector<size_t>{ 3, wait, 4, 6, 7 } && order.previous(4) == wait && order.next(3) == wait,
            "causal stream order retains an inserted logical wait despite the original bypass");
    require(&source.device_stream_order(7) == &order, "one physical stream reuses its proven order");
    model::HiCacheState state(timing_config());
    runtime::HiCacheWrites writes(state);
    writes.add_source(source, fixture.record);
    require(writes.source_start_record(source, 42) == 2 && writes.source_completion(42) == 7,
            "write binding accepts a logical wait and retains its producer and completion Records");
    graph.disable_edge(onward);
    const auto reject = [&] {
        hicache_timing_fixture::require_throws<std::runtime_error>([&] { (void)patch::HiCacheSourceDagIndex(graph).device_stream_order(4); },
                                                                   "genuinely unordered or cyclic stream nodes cannot be sorted by IDs or device times");
    };
    reject(); // Wait and payload now have no relative dependency.
    graph.mutable_edge(onward).active = true;
    graph.add_edge(wait, 3, core::DagEdgeKind::Stream);
    reject();
}

void existing_write(uint64_t compute_us, uint64_t later_submit_us, bool cancelled, bool replace_consumer = false) {
    model::HiCacheState state(timing_config());
    state.begin_formal_window(true);
    auto fixture = observed_write(compute_us, later_submit_us);
    std::ranges::reverse(fixture.record.device_transfer_node_ids); // Tied timestamps cannot define stream order.
    runtime::HiCacheWrites writes(state);
    writes.add_source(patch::HiCacheSourceDagIndex(fixture.graph), fixture.record);
    size_t consumer = 9;
    if (replace_consumer) {
        fixture.graph.mutable_node(9).active = false;
        for (size_t i = 0; i < fixture.graph.edge_count(); ++i)
            if (fixture.graph.edge(i).src == 9 || fixture.graph.edge(i).dst == 9) fixture.graph.mutable_edge(i).active = false;
        consumer = fixture.graph.add_synthetic_node({ .name = "replacement consumer", .is_cpu = true, .lane_key = "wait", .duration = 1 });
        fixture.graph.mutable_node(consumer).counts_toward_e2e = true;
        fixture.graph.add_edge(7, consumer, core::DagEdgeKind::Sync);
    }
    writes.rebind_consumers(patch::HiCacheSourceDagIndex(fixture.graph));
    auto fact = request_fact("cache_lifecycle_commit", "r", 100, 0);
    fact.lifecycle_kind = "finished";
    std::optional<size_t> done;
    (void)simulation::run_topological_simulation(
        fixture.graph,
        [&](size_t node, uint64_t, uint64_t original) { return writes.duration(node, original); },
        [&](size_t node, uint64_t at, simulation::FutureDag & future) {
            fact.ts = 100 + at;
            writes.advance(node, fact.ts);
            if (node == 0) {
                std::optional<model::HiCacheDeviceWrite> operation;
                if (!cancelled) {
                    state.apply_fact(fact, HiCacheFactRole::CacheLifecycleCommit);
                    const auto pending = state.pending_device_writes(fact);
                    require(pending.size() == 1 && pending.front().schedule.duration_us == 20, "target state provides one measured-service write");
                    operation = pending.front();
                }
                done = writes.submit_source(42, fact, operation, future);
            }
            if (node == consumer && !cancelled) {
                require(writes.completed() == 1 && state.write_completion_count(fact) == 1, "completion callback runs before the source wait consumer");
                for (const auto & effect : state.effect_decision_ledger().decisions)
                    if (effect.effect_type == model::HiCacheEffectType::CommitDeviceToHost)
                        require(effect.completed_page_count == 0, "ordinary DMA completion cannot publish host pages before acknowledgement");
                state.acknowledge_writes(fact, 1);
                require(state.write_completion_count(fact) == 0, "explicit acknowledgement drains the completed write");
            }
        });
    const auto & graph = fixture.graph;
    const uint64_t first = cancelled ? 0 : 6, second = cancelled ? 0 : 14;
    const auto expected = std::max(std::max(uint64_t{ 5 }, compute_us) + first, 5 + later_submit_us) + second + 2;
    require(graph.node(4).duration == first && graph.node(6).duration == second, "byte split replaces rather than adds to source costs and conserves rounding");
    require(graph.node(consumer).simulation_start == expected, "consumer waits for CPU submissions, compute readiness, both transfers and finish Record");
    require(graph.node(8).completion_time == 8, "ordinary write submission must not block unrelated host continuation");
    require(cancelled ? !done && writes.completed() == 0 : done && graph.node(*done).completion_time == expected,
            "cancelled writes emit no state completion; retained writes use actual Record completion");
    require_static_replay(fixture.graph, "write graph must replay identically without execution callbacks");
}
void new_writes_resume_capacity(uint64_t compute_us) {
    auto config = timing_config();
    config.write_policy = "write_back";
    model::HiCacheState state(config);
    for (uint32_t i = 0; i < 4; ++i) {
        auto seed = request_fact("cache_lifecycle_commit", "seed" + std::to_string(i), i + 1, i * 16);
        seed.token_count = seed.full_path_span.end = seed.full_path_span.token_count = 16;
        seed.full_path_tokens.resize(16);
        seed.lifecycle_kind = "finished";
        state.apply_fact(seed, HiCacheFactRole::CacheLifecycleCommit, false);
        state.finalize();
    }
    state.begin_formal_window(true);
    core::DagGraph graph;
    const auto compute = graph.add_synthetic_node({ .name = "data production", .is_cpu = false, .duration = compute_us });
    const auto host = graph.add_synthetic_node({ .name = "host submission", .duration = 5 });
    const auto next = graph.add_synthetic_node({ .name = "CPU after capacity", .duration = 3, .counts_toward_e2e = true });
    graph.add_edge(host, next, core::DagEdgeKind::Mutation);
    const auto submit = graph.add_synthetic_node({
        .name = "earlier compute submission",
        .lane_key = "main",
        .observed_point = core::DagObservedPoint{ "worker", "main", 90, 0 }
    });
    graph.add_edge(submit, compute, core::DagEdgeKind::Correlation);
    const auto position = runtime::observe_write_stream_position(patch::HiCacheSourceDagIndex(graph), compute, "worker", "main", 100);
    require(position.before == compute && !position.after, "new write uses the stream frontier proved by earlier CPU submission");
    auto fact = request_fact("cache_extend_input", "active", 100, 2'000);
    fact.is_start = true;
    fact.is_end = false;
    fact.token_count = fact.full_path_span.end = fact.full_path_span.token_count = 16;
    fact.full_path_tokens.resize(16);
    fact.batch_paths = {
        { .request_id = fact.request_id, .full_path_span = fact.full_path_span, .full_path_tokens = fact.full_path_tokens, .token_count = 16 }
    };
    runtime::HiCacheWrites writes(state);
    std::optional<size_t> resume;
    std::vector<std::pair<size_t, size_t>> expanded_streams;
    (void)simulation::run_topological_simulation(graph, {}, [&](size_t node, uint64_t at, simulation::FutureDag & future) {
        fact.ts = 100 + at;
        writes.advance(node, fact.ts);
        if (node == host) {
            state.apply_fact(fact, HiCacheFactRole::CacheExtendInput);
            const auto pending = state.pending_device_writes(fact);
            require(state.allocation_pending(fact) && pending.size() == 2, "new branch has two state-derived dirty victims");
            hicache_timing_fixture::require_throws<std::logic_error>([&] { (void)writes.submit_expanded(fact, pending.back(), compute, compute, future); },
                                                                     "device submission order must not reverse the controller's target queue");
            for (const auto & write : pending) {
                const auto start = future.append({ .name = "new write Wait", .is_cpu = false, .duration = 2 });
                const auto payload = future.append({ .name = "new write DMA", .is_cpu = false, .duration = write.schedule.duration_us });
                const auto record = future.append({ .name = "new write Record", .is_cpu = false, .duration = 1 });
                future.depend(*position.before, start);
                future.depend(start, payload);
                future.depend(payload, record);
                resume = writes.submit_expanded(fact, write, start, record, future);
                expanded_streams.emplace_back(start, record);
            }
            future.depend(*resume, next);
        }
        if (resume == node) {
            require(writes.completed() == 2 && state.prefill_work_items().empty(), "all serial device writes finish before capacity is released");
            state.resume_allocation(fact);
        }
        if (node == next) require(!state.allocation_pending(fact) && state.prefill_work_items().size() == 1, "CPU cannot bypass resumed allocation");
    });
    require(graph.node(next).completion_time == std::max(uint64_t{ 5 }, compute_us) + 20 + 3 + 6,
            "source-empty writes wait for host submission, data readiness and their shared device queue");
    require(graph.node(expanded_streams[1].first).simulation_start >= graph.node(expanded_streams[0].second).completion_time,
            "the next write Wait cannot overtake the previous completion Record");
    require_static_replay(graph, "write graph must replay identically without execution callbacks");
}
void invalid_layout() {
    model::HiCacheState state(timing_config());
    const auto rejects = [&](WriteGraph fixture) {
        runtime::HiCacheWrites writes(state);
        hicache_timing_fixture::require_throws<std::exception>([&] { writes.add_source(patch::HiCacheSourceDagIndex(fixture.graph), fixture.record); },
                                                               "unproven source transfer ownership must not be guessed");
    };
    auto wrong_record = observed_write(0, 0);
    wrong_record.graph.mutable_node(7).submit_ts = 200;
    rejects(std::move(wrong_record));
    auto interrupted = observed_write(0, 0);
    interrupted.record.device_transfer_node_ids = { 4 };
    rejects(std::move(interrupted));
    auto missing_bytes = observed_write(0, 0);
    missing_bytes.graph.mutable_events().at(missing_bytes.graph.node(4).event_index).set_arg("size(B)", "");
    rejects(std::move(missing_bytes));
}
void template_compute_stream_is_proven() {
    model::HiCacheState state(timing_config());
    auto fixture = observed_write(30, 0);
    fixture.graph.mutable_node(2).submit_ts = 102;
    runtime::HiCacheWrites writes(state);
    writes.add_source(patch::HiCacheSourceDagIndex(fixture.graph), fixture.record);
    require(writes.source_start_record(patch::HiCacheSourceDagIndex(fixture.graph), 42) == 2, "write template uses the compute Record actually waited on");

    fixture.graph.mutable_node(2).submit_ts = 99;
    hicache_timing_fixture::require_throws<std::runtime_error>([&] { (void)writes.source_start_record(patch::HiCacheSourceDagIndex(fixture.graph), 42); },
                                                               "an earlier call's Record is not a template for this write");

    fixture.graph.mutable_node(2).submit_ts = 102;
    const auto other = fixture.graph.add_synthetic_node({ .name = "EVENT_RECORD", .is_cpu = false, .lane_key = "another compute" });
    fixture.graph.mutable_node(other).submit_ts = 103;
    fixture.graph.add_edge(other, 3, core::DagEdgeKind::Sync);
    hicache_timing_fixture::require_throws<std::runtime_error>([&] { (void)writes.source_start_record(patch::HiCacheSourceDagIndex(fixture.graph), 42); },
                                                               "two compute streams cannot be collapsed to the nearest one");
}
void unassigned_source_is_not_free() {
    model::HiCacheState state(timing_config());
    auto fixture = observed_write(0, 0);
    runtime::HiCacheWrites writes(state);
    writes.add_source(patch::HiCacheSourceDagIndex(fixture.graph), fixture.record);
    bool rejected = false;
    try {
        (void)simulation::run_topological_simulation(fixture.graph, [&](size_t node, uint64_t, uint64_t duration) { return writes.duration(node, duration); });
    }
    catch (const std::logic_error &) {
        rejected = true;
    }
    require(rejected, "a source payload without a target decision cannot silently keep source cost or become free");
}
void stream_position_uses_submission_order() {
    core::DagGraph graph;
    for (size_t i = 0; i < 3; ++i) {
        const auto cpu = graph.add_synthetic_node({
            .name = "submit",
            .lane_key = "main",
            .duration = 2,
            .observed_point = core::DagObservedPoint{ "worker", "main", 100 + i * 10, 0 }
        });
        const auto device = graph.add_synthetic_node({
            .name = "queued compute",
            .is_cpu = false,
            .lane_key = "compute",
            .duration = 50,
            .observed_point = core::DagObservedPoint{ "worker", "device", 1'000 + i * 50, 0 }
        });
        graph.add_edge(cpu, device, core::DagEdgeKind::Correlation);
        if (i) graph.add_edge(device - 2, device, core::DagEdgeKind::Stream);
    }
    const auto observe = [&](uint64_t at) { return runtime::observe_write_stream_position(patch::HiCacheSourceDagIndex(graph), 1, "worker", "main", at); };
    const auto middle = observe(115);
    require(middle.before == 3 && middle.after == 5, "stream insertion follows CPU submissions even when every GPU event is later");
    require(!observe(99).before && observe(99).after == 1, "insertion before the first submission has no prior data work");
    require(observe(123).before == 5 && !observe(123).after, "insertion after the last submission retains its data dependency");
    const auto rejects = [&](uint64_t at) {
        hicache_timing_fixture::require_throws<std::runtime_error>([&] { (void)observe(at); },
                                                                   "ambiguous submission evidence cannot choose a convenient GPU timestamp");
    };
    rejects(111);
    graph.mutable_event_for_node(0).tid = "unobserved submitter";
    require(observe(115).before == 3, "stream predecessor of a proven earlier submission is also before the insertion");
    rejects(105); // Unknown node lies at the insertion cut, not in a proven prefix.
    graph.mutable_event_for_node(0).tid = "main";
    graph.mutable_event_for_node(4).tid = "unobserved submitter";
    require(observe(105).after == 3, "stream successor of a proven later submission is also after the insertion");
    rejects(115);
    graph.mutable_event_for_node(4).tid = "main";
    graph.mutable_event_for_node(2).tid = "another thread";
    rejects(115);
    graph.mutable_event_for_node(2).tid = "main";
    graph.mutable_event_for_node(0).ts = 130;
    rejects(115);
}
} // namespace

void check_write_transfer_execution() {
    redundant_stream_edges_keep_one_order();
    template_compute_stream_is_proven();
    stream_position_uses_submission_order();
    existing_write(0, 0, false);
    existing_write(30, 0, false);
    existing_write(0, 40, false);
    existing_write(30, 40, true);
    existing_write(30, 40, false, true);
    new_writes_resume_capacity(0);
    new_writes_resume_capacity(30);
    invalid_layout();
    unassigned_source_is_not_free();
}
