#include "hicache_timing_fixture.hpp"
#include "markov/trace_graph/modules/hicache/missing_cost.hpp"
#include "markov/trace_graph/modules/hicache/model/prefetch_check_execution.hpp"
#include "markov/trace_graph/modules/hicache/model/prefetch_execution.hpp"
#include "markov/trace_graph/modules/hicache/model/prefetch_wait_calibration.hpp"
#include "markov/trace_graph/modules/hicache/model/prefetch_wait_execution.hpp"
#include <algorithm>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
using namespace hicache_timing_fixture;

namespace {
model::PrefetchCheckTiming check_timing() {
    model::PrefetchCheckTiming timing;
    for (const int rank : { 0, 1 }) {
        timing.cpu[rank] = { 0, 0, 0, 0, 0, 0 };
        timing.state_max.calls[rank] = {
            .entry_ranks = { 0, 1 },
            .worker_remainder = 8
        };
        timing.completion_min.calls[rank] = {
            .entry_ranks = { 0, 1 },
            .worker_remainder = 8
        };
    }
    return timing;
}

model::PrefetchSchedulerTiming scheduler_body() {
    // Explicit synthetic timing: four instantaneous communications, five us of
    // local work. No incoming requests or outstanding writes/loads in this fixture.
    model::PrefetchSchedulerTiming body;
    for (const auto action : { model::PrefetchSchedulerAction::Receive,
                               model::PrefetchSchedulerAction::WriteCompletion,
                               model::PrefetchSchedulerAction::LoadCompletion,
                               model::PrefetchSchedulerAction::StorageDrain }) {
        model::PrefetchSchedulerStepTiming step{ .action = action };
        for (const int rank : { 0, 1 }) {
            body.tail[rank] = {};
            step.before[rank].cpu_us = body.steps.empty() ? 5 : 0;
            step.after[rank] = {};
            step.communication.calls[rank] = {
                .entry_ranks = { 0, 1 },
                .worker_remainder = 0
            };
        }
        body.steps.push_back(std::move(step));
    }
    return body;
}

void check_and_compute_run_together() {
    struct Case {
        std::string policy;
        uint64_t first, second, pages, rounds, e2e;
        bool cancelled = false;
        bool ordinary_cost = false;
        uint64_t sample_prefix = 0;
    };
    for (const auto c : std::vector<Case>{
             { "wait_complete", 23, 35, 2, 2, 116 },
             { "wait_complete", 35, 35, 2, 1, 103 },
             { "timeout", 23, 35, 2, 1, 103 },
             { "best_effort", 23, 27, 0, 1, 131 },
             { "best_effort", 31, 31, 2, 1, 91 },
             { "wait_complete", 23, 35, 0, 1, 147, true },
             { "wait_complete", 23, 35, 2, 2, 132, false, true },
             { "wait_complete", 23, 23, 2, 1, 103, false, false, 12 }
    }) {
        auto config = timing_config();
        config.prefetch_policy = c.policy;
        config.prefetch_timeout_configured = true;
        config.prefetch_timeout_base_sec = config.prefetch_timeout_max_sec = .000022;
        config.io_cost.storage_batch_pages = 2;
        config.io_cost.service_models.at("prefetch").stages = { 10, 0, 5, 0, 10, 0 };
        model::HiCacheState state(config);
        seed_storage(state, "rank:0:cache");
        seed_storage(state, "rank:1:cache");
        state.begin_formal_window(true);
        const uint64_t origin = 200'000;
        std::map<int, HiCacheFact> facts;
        core::DagGraph graph;
        const auto launch = graph.add_synthetic_node({ .name = "queries returned" });
        const auto before_cancel = graph.add_synthetic_node({ .name = "before independent cancellation", .duration = 8 });
        const auto cancel = graph.add_synthetic_node({ .name = "independent cancellation" });
        graph.add_edge(launch, before_cancel, core::DagEdgeKind::Mutation);
        graph.add_edge(before_cancel, cancel, core::DagEdgeKind::Mutation);
        CpuRankNodes entries, lookups, extends;
        for (const int rank : { 0, 1 }) {
            auto fact = request_fact("prefetch_candidate_anchor", "request", origin, 0);
            fact.cache_scope = "rank:" + std::to_string(rank) + ":cache";
            fact.source_node_id += rank;
            state.apply_fact(fact, HiCacheFactRole::PrefetchCandidateAnchor);
            complete_query(state, fact);
            facts[rank] = fact;
            const auto before = graph.add_synthetic_node({ .name = "ordinary foreground work", .duration = rank ? c.second : c.first });
            entries[rank] = graph.add_synthetic_node({ .name = "check entry" });
            lookups[rank] = graph.add_synthetic_node({ .name = "loadback" });
            extends[rank] = graph.add_synthetic_node({ .name = "prefill", .counts_toward_e2e = true });
            for (const auto id : { before, entries[rank], lookups[rank], extends[rank] }) graph.mutable_node(id).gpu_id = rank;
            graph.add_edge(launch, before, core::DagEdgeKind::Mutation);
            graph.add_edge(before, entries[rank], core::DagEdgeKind::Mutation);
            graph.add_edge(entries[rank], lookups[rank], core::DagEdgeKind::Mutation);
            graph.add_edge(lookups[rank], extends[rank], core::DagEdgeKind::Mutation);
        }
        model::HiCachePrefetchExecution worker(state, config);
        auto timing = check_timing();
        if (c.ordinary_cost)
            for (auto & [rank, cpu] : timing.cpu) cpu = { 1, 3, 2, 3, 4, 5 };
        for (auto & [rank, cpu] : timing.cpu) cpu.entry_to_check = c.sample_prefix;
        size_t scheduler_actions = 0, finished = 0;
        model::HiCachePrefetchWaitExecution waiting(
            state,
            facts,
            timing,
            c.policy == "best_effort" ? model::PrefetchSchedulerTiming{} : scheduler_body(),
            c.policy,
            [&](model::PrefetchSchedulerAction action, const model::PrefetchSchedulerBoundary &, uint64_t, simulation::FutureDag &) {
                require(action != model::PrefetchSchedulerAction::StorageDrain, "storage drains belong to the shared state executor");
                ++scheduler_actions; // The fixture has no receive/write/load work to apply.
            });
        CpuRankNodes returned;
        std::map<int, std::optional<model::HiCacheLoadbackBatch>> loads;
        const auto result = simulation::run_topological_simulation(
            graph,
            [&](size_t id, uint64_t time, uint64_t duration) {
                for (const auto & [rank, original] : facts) {
                    auto fact = original;
                    fact.ts = origin + time;
                    fact.source_node_id = fact.source_event_index = 1'000 + id;
                    if (id == lookups.at(rank)) {
                        require(state.prefetch_operation(fact)->completed_pages.size() == c.pages, "foreground lookup must see the executed cross-rank MIN");
                        fact.role = "cache_lookup_input";
                        state.apply_fact(fact, HiCacheFactRole::CacheLookupInput);
                        // This fixture executes the load service on this node;
                        // its successor is the explicit device-completion boundary.
                        loads[rank] = state.submit_loadbacks(fact);
                        return loads.at(rank) ? loads.at(rank)->io_schedule.ready_ts - fact.ts : 0;
                    }
                    if (id == extends.at(rank)) {
                        if (loads.at(rank)) state.complete_loadback_batch(fact, loads.at(rank)->id);
                        fact.role = "cache_extend_input";
                        fact.is_start = true;
                        fact.is_end = false;
                        fact.token_count = fact.full_path_span.end = fact.full_path_span.token_count = 48;
                        for (uint32_t token = 32; token < 48; ++token) fact.full_path_tokens.push_back({ { token } });
                        fact.batch_paths = {
                            { .request_id = fact.request_id,
                             .full_path_span = fact.full_path_span,
                             .full_path_tokens = fact.full_path_tokens,
                             .token_count = 48 }
                        };
                        state.apply_fact(fact, HiCacheFactRole::CacheExtendInput);
                        const auto tokens = state.prefill_work_items().back().prefill_token_count;
                        require(tokens == 48 - c.pages * 16, "actual cache visibility must change subsequent Prefill work");
                        return tokens * 2;
                    }
                }
                return duration;
            },
            [&](size_t id, uint64_t time, simulation::FutureDag & future) {
                worker.advance(id, origin + time, future);
                if (c.cancelled && id == cancel) {
                    auto fact = facts.at(0);
                    fact.ts = origin + time;
                    (void)state.stop_prefetch(fact);
                }
                if (id == launch) {
                    for (const auto & [rank, fact] : facts) (void)worker.enqueue(fact, future);
                    returned = waiting.start(entries, future);
                    for (const auto & [rank, node] : returned) future.depend(node, lookups.at(rank));
                }
                if (waiting.advance(id, origin + time, future)) ++finished;
                require(waiting.checks_issued() < 4, "check loop did not make progress");
            });
        require(waiting.checks_issued() == c.rounds, "a false snapshot must retry even if I/O finishes during its MAX");
        require(finished == 2 && scheduler_actions == (c.rounds - 1) * 12, "each retry must dispatch all scheduler actions and return both ranks");
        if (result.e2e_us != c.e2e)
            throw std::runtime_error(c.policy + " control/loadback/Prefill expected " + std::to_string(c.e2e) + ", got " + std::to_string(result.e2e_us));
        for (const auto & [rank, node] : returned) require(graph.node(node).gpu_id == rank, "collective-created work must retain participant rank");

        const auto replay = hicache_timing_fixture::require_static_replay(graph, "materialized control must preserve every execution time");
        require(replay.e2e_us == result.e2e_us, "executed control must remain in the materialized DAG");
    }
}

void query_miss_is_settled_by_the_loop() {
    auto config = timing_config();
    model::HiCacheState state(config);
    for (const int rank : { 0, 1 }) seed_storage(state, "rank:" + std::to_string(rank) + ":cache");
    state.begin_formal_window(true);
    std::map<int, HiCacheFact> facts;
    core::DagGraph graph;
    const auto root = graph.add_synthetic_node({ .name = "query MIN returned zero", .counts_toward_e2e = true });
    CpuRankNodes entries, returned;
    for (const int rank : { 0, 1 }) {
        auto backup = request_fact("cache_lifecycle_commit", "new-backup", 200'000, 5'000);
        backup.cache_scope = "rank:" + std::to_string(rank) + ":cache";
        backup.lifecycle_kind = "finished";
        state.apply_fact(backup, HiCacheFactRole::CacheLifecycleCommit);
        auto fact = request_fact("prefetch_candidate_anchor", "missing-prefix", 200'000, 1'024);
        fact.cache_scope = "rank:" + std::to_string(rank) + ":cache";
        state.apply_fact(fact, HiCacheFactRole::PrefetchCandidateAnchor);
        require(state.query_prefetch_storage(fact) == 0, "query fixture must miss the stored prefix");
        state.complete_prefetch_query(fact, 0);
        require(state.prefetch_queue_sizes(fact).revoked_operations == 1, "query miss queues revocation rather than completing the foreground check");
        facts[rank] = fact;
        const auto before = graph.add_synthetic_node({ .name = "ordinary work", .duration = 10 });
        entries[rank] = graph.add_synthetic_node({ .name = "first progress entry" });
        graph.mutable_node(entries[rank]).gpu_id = rank;
        graph.add_edge(root, before, core::DagEdgeKind::Mutation);
        graph.add_edge(before, entries[rank], core::DagEdgeKind::Mutation);
    }
    auto timing = check_timing();
    for (auto & [rank, cpu] : timing.cpu) cpu.no_operation_return = 7;
    size_t actions = 0;
    std::map<int, uint64_t> write_samples;
    model::HiCachePrefetchWaitExecution waiting(
        state,
        facts,
        timing,
        scheduler_body(),
        "wait_complete",
        [&](model::PrefetchSchedulerAction action, const model::PrefetchSchedulerBoundary & boundary, uint64_t time, simulation::FutureDag &) {
            ++actions; // Receive/load are empty in this fixture; write completion is not.
            if (action != model::PrefetchSchedulerAction::WriteCompletion) return;
            auto fact = facts.at(boundary.rank);
            fact.ts = time;
            complete_test_writes(state, fact);
            if (!boundary.apply) write_samples[boundary.rank] = state.write_completion_count(fact);
            else {
                require(write_samples.size() == 2, "write MIN must wait for both samples");
                state.acknowledge_writes(fact, std::min(write_samples.at(0), write_samples.at(1)));
            }
        });
    (void)simulation::run_topological_simulation(graph, {}, [&](size_t node, uint64_t at, simulation::FutureDag & future) {
        if (node == root) returned = waiting.start(entries, future);
        (void)waiting.advance(node, 200'000 + at, future);
        require(waiting.checks_issued() <= 2, "query miss must be resolved by scheduler revocation, not endless polling");
    });
    require(waiting.checks_issued() == 2 && actions == 12, "query miss needs one scheduler body then a local early return");
    for (const auto & [rank, original] : facts) {
        auto fact = original;
        fact.ts += 30;
        const auto queues = state.prefetch_queue_sizes(fact);
        require(!state.sample_prefetch_check(fact).ongoing && !queues.revoked_operations && !queues.released_pages,
                "loop drain must revoke and release the actual state queues");
        require(graph.node(returned.at(rank)).completion_time == 30, "final return must include measured inactive-path cost after drain");
        require(write_samples.at(rank) == 1 && state.write_completion_count(fact) == 0 && !queues.backup_acks,
                "scheduler body must consume its write ACK without inventing storage completion");
    }
    hicache_timing_fixture::require_static_replay(graph, "state-driven termination must survive static replay");
}

void absent_prefetch_returns_locally() {
    for (const auto policy : { "wait_complete", "best_effort" }) {
        auto config = timing_config();
        config.prefetch_policy = policy;
        config.io_cost.service_models.at("prefetch").stages = { 10, 0, 5, 0, 10, 0 };
        for (const int mode : { 0, 1, 2, 3, 4 }) {
            const bool missing_cost = mode == 1, mixed = mode == 2 || mode == 3, missing_active = mode == 4;
            model::HiCacheState state(config);
            seed_storage(state, "rank:0:cache");
            seed_storage(state, "rank:1:cache");
            state.begin_formal_window(true);
            core::DagGraph graph;
            const auto root = graph.add_synthetic_node({ .name = "before local progress", .counts_toward_e2e = true });
            CpuRankNodes entries, returned;
            std::map<int, HiCacheFact> facts;
            model::PrefetchCheckTiming timing; // No collective observations needed on an inactive path.
            if (mixed) timing = check_timing();
            for (const auto [rank, delay] : CpuRankTimes{
                     { 0,   5 },
                     { 1, 105 }
            }) {
                const auto before = graph.add_synthetic_node({ .name = "ordinary CPU", .duration = delay });
                entries[rank] = graph.add_synthetic_node({ .name = "local progress entry" });
                graph.mutable_node(entries[rank]).gpu_id = rank;
                graph.add_edge(root, before, core::DagEdgeKind::Mutation);
                graph.add_edge(before, entries[rank], core::DagEdgeKind::Mutation);
                facts[rank] = request_fact("prefetch_candidate_anchor", "absent", 200'000, 0);
                facts[rank].cache_scope = "rank:" + std::to_string(rank) + ":cache";
                if (missing_active || (mixed && rank == mode - 2)) {
                    state.apply_fact(facts.at(rank), HiCacheFactRole::PrefetchCandidateAnchor);
                    complete_query(state, facts.at(rank));
                }
                if (!mixed) timing.cpu[rank] = {};
                if (!missing_cost) timing.cpu[rank].no_operation_return = 7 + rank;
            }
            model::HiCachePrefetchCheckExecution check(state, facts, timing, policy);
            size_t ready = 0;
            bool rejected = false;
            try {
                (void)simulation::run_topological_simulation(graph, {}, [&](size_t id, uint64_t time, simulation::FutureDag & future) {
                    if (id == root) returned = check.start(entries, future);
                    if (const auto result = check.advance(id, 200'000 + time, future)) {
                        require(*result, "absent operations return ready without a collective");
                        ++ready;
                    }
                });
            }
            catch (const MissingCostEvidence & error) {
                rejected = error.requirement.at("component") == "execution_control/prefetch"
                           && ((missing_active && error.requirement.at("coordinates").at("program") == policy)
                               || (missing_cost && error.requirement.at("coordinates").at("program") == "local_return"));
                if (!rejected) throw;
            }
            catch (const std::logic_error & error) {
                rejected = mixed && std::string(error.what()).find("mix local early returns") != std::string::npos;
                if (!rejected) throw;
            }
            require(rejected == (missing_cost || mixed || missing_active), "missing cost or inconsistent participation must not become a successful return");
            if (rejected) continue;
            require(ready == 2 && graph.node(returned.at(0)).completion_time == 12 && graph.node(returned.at(1)).completion_time == 113,
                    "early local return must not wait for the late peer");
            for (const auto & node : graph.nodes())
                require(graph.event_for_node(node.id).cat != "cpu_collective", "inactive progress must not emit communication service");
            hicache_timing_fixture::require_static_replay(graph, "local early-return costs must survive static replay");
        }
    }
}

void pending_query_keeps_empty_hash_list() {
    auto config = timing_config();
    config.prefetch_policy = "timeout";
    config.prefetch_timeout_configured = true;
    config.prefetch_timeout_base_sec = .000005;
    config.prefetch_timeout_per_ki_token_sec = .001;
    config.prefetch_timeout_max_sec = 1;
    config.io_cost.service_models.at("prefetch").stages = { 10, 0, 5, 0, 10, 0 };
    model::HiCacheState state(config);
    seed_storage(state);
    state.begin_formal_window(true);
    auto fact = request_fact("prefetch_candidate_anchor", "request", 200'000, 0);
    state.apply_fact(fact, HiCacheFactRole::PrefetchCandidateAnchor);
    fact.ts += 1;
    require(state.query_prefetch_storage(fact) == 2, "fixture must find local storage hits");
    fact.ts = 200'005;
    require(!state.sample_prefetch_check(fact).can_stop, "timeout uses strict greater-than");
    fact.ts = 200'006;
    require(state.sample_prefetch_check(fact).can_stop, "unreturned query cannot extend timeout using its unpublished hash list");
    fact.ts = 200'007;
    state.complete_prefetch_query(fact, 2);
    require(!state.sample_prefetch_check(fact).can_stop, "accepted query updates the token-dependent timeout");
    fact.ts = 200'008;
    require(state.stop_prefetch(fact) == 0, "early cancellation publishes no page");
    fact.ts = 200'010;
    require(state.sample_prefetch_check(fact).terminated && state.stop_prefetch(fact) == 0, "foreground stop after cancellation is idempotent");
    require(state.prefetch_operation(fact)->execution_stop_ts == 200'008, "repeated stop cannot move physical cancellation later");

    model::HiCacheState pending(config);
    seed_storage(pending);
    pending.begin_formal_window(true);
    fact = request_fact("prefetch_candidate_anchor", "pending", 200'000, 0);
    pending.apply_fact(fact, HiCacheFactRole::PrefetchCandidateAnchor);
    fact.ts = 200'006;
    (void)pending.stop_prefetch(fact);
    pending.publish_prefetch(fact, 0);
    require(pending.prefetch_operation(fact)->timed_out && pending.prefetch_operation(fact)->timeout_deadline_ts == 200'005,
            "a still-empty query is not completed I/O; its timeout must remain visible in the decision");
}
} // namespace
void check_prefetch_check_execution() {
    model::PrefetchQueryCalibration query_calibration;
    query_calibration.source_manifest = "independent/profile_manifest.json";
    query_calibration.timing.agreement = check_timing().completion_min;
    for (const auto & [rank, call] : query_calibration.timing.agreement.calls) query_calibration.timing.cpu[rank] = { 12, 2, 3 };
    const auto query_json = model::encode_prefetch_query_calibration(query_calibration);
    require(model::encode_prefetch_query_calibration(model::decode_prefetch_query_calibration(query_json)) == query_json,
            "independent query costs preserve CPU segments and communication, without source node ids");
    for (int invalid = 0; invalid < 5; ++invalid) {
        auto document = query_json;
        if (invalid == 0) document["role"] = "target";
        if (invalid == 1) document["operation"] = "prefetch_stop";
        if (invalid == 2) document["cpu"][0]["before_sample"] = -1;
        if (invalid == 3) document["agreement"].erase(0);
        if (invalid == 4) document["agreement"][0]["entry_ranks"] = { 0, 0 };
        hicache_timing_fixture::require_throws<std::invalid_argument>([&] { (void)model::decode_prefetch_query_calibration(document); },
                                                                      "query costs cannot borrow another operation or omit collective peers");
    }
    const model::PrefetchWaitCalibration calibration{
        "independent/profile_manifest.json",
        "calibration-request",
        "wait_complete",
        { check_timing(), scheduler_body() }
    };
    const auto encoded = model::encode_prefetch_wait_calibration(calibration);
    require(model::encode_prefetch_wait_calibration(model::decode_prefetch_wait_calibration(encoded)) == encoded,
            "portable calibration JSON must preserve all observed timing and provenance");
    auto with_local_return = calibration;
    for (auto & [rank, cpu] : with_local_return.timing.check.cpu) cpu.no_operation_return = 7 + rank;
    const auto local_roundtrip = model::decode_prefetch_wait_calibration(model::encode_prefetch_wait_calibration(with_local_return));
    for (const auto & [rank, cpu] : local_roundtrip.timing.check.cpu)
        require(cpu.no_operation_return == std::optional<uint64_t>{ 7 + static_cast<uint64_t>(rank) },
                "independent local-return cost survives per-rank calibration roundtrip");
    auto immediate = calibration;
    immediate.source_policy = "best_effort";
    immediate.timing.scheduler = {};
    immediate.timing.check.state_max = {};
    for (auto & [rank, cpu] : immediate.timing.check.cpu) {
        cpu.check_to_max.reset();
        cpu.max_to_false_return.reset();
        cpu.max_to_stop.reset();
    }
    const auto stop_json = model::encode_prefetch_wait_calibration(immediate);
    require(model::encode_prefetch_wait_calibration(model::decode_prefetch_wait_calibration(stop_json)) == stop_json,
            "immediate stop preserves its real MIN and costs without invented MAX or retry body");
    for (int invalid = 0; invalid < 5; ++invalid) {
        auto document = stop_json;
        if (invalid == 0) document["cpu"][0]["best_effort_to_stop"] = nullptr;
        if (invalid == 1) document["state_max"] = encoded["state_max"];
        if (invalid == 2) document["steps"] = encoded["steps"];
        if (invalid == 3) document["completion_min"].erase(0);
        if (invalid == 4) document["source_policy"] = "wait_complete";
        hicache_timing_fixture::require_throws<std::invalid_argument>([&] { (void)model::decode_prefetch_wait_calibration(document); },
                                                                      "immediate-stop calibration cannot hide a missing rank, branch cost or policy mismatch");
    }
    for (int invalid = 0; invalid < 6; ++invalid) {
        auto document = encoded;
        if (invalid == 0) document["cpu"][0]["entry_to_check"] = -1;
        if (invalid == 1) document["source_manifest"] = "";
        if (invalid == 2) document["tail"].erase("1");
        if (invalid == 3) document["cpu"][0]["max_to_stop"] = nullptr;
        if (invalid == 4) document["tail"]["0"]["gap_us"] = -1;
        if (invalid == 5) document["tail"]["0"] = 10;
        hicache_timing_fixture::require_throws<std::invalid_argument>([&] { (void)model::decode_prefetch_wait_calibration(document); },
                                                                      "invalid calibration cannot silently supply zero or partial costs");
    }
    absent_prefetch_returns_locally();
    query_miss_is_settled_by_the_loop();
    check_and_compute_run_together();
    pending_query_keeps_empty_hash_list();
}
