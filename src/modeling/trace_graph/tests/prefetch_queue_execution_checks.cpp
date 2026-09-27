#include "hicache_timing_fixture.hpp"
#include "markov/trace_graph/modules/hicache/model/prefetch_check_execution.hpp"
#include "markov/trace_graph/modules/hicache/model/prefetch_execution.hpp"
#include "markov/trace_graph/modules/hicache/model/prefetch_queue_execution.hpp"
#include <memory>
#include <stdexcept>
using namespace hicache_timing_fixture;

namespace {
constexpr uint64_t origin = 200'000;
model::PrefetchQueueTiming queue_timing() {
    model::PrefetchQueueTiming timing;
    for (int rank : { 0, 1 }) {
        timing.cpu[rank] = { rank ? 8UL : 4UL, 2, 1 };
        timing.agreement.calls[rank] = {
            .entry_ranks = { 0, 1 },
            .envelope_remainder_us = rank ? 5UL : 3UL
        };
    }
    return timing;
}
std::map<int, HiCacheFact> candidates(model::HiCacheState & state, const std::string & request, uint32_t token, uint64_t at) {
    std::map<int, HiCacheFact> facts;
    for (int rank : { 0, 1 }) {
        auto fact = request_fact("prefetch_candidate_anchor", request, at, token);
        fact.cache_scope = "rank:" + std::to_string(rank) + ":cache";
        facts[rank] = fact;
        state.apply_fact(fact, HiCacheFactRole::PrefetchCandidateAnchor);
    }
    return facts;
}
CpuRankNodes entries(core::DagGraph & graph, size_t root, uint64_t delay) {
    CpuRankNodes result;
    for (int rank : { 0, 1 }) {
        const auto prior = graph.add_synthetic_node({ .name = "ordinary work", .duration = delay });
        result[rank] = graph.add_synthetic_node({ .name = "entry" });
        graph.mutable_node(result[rank]).gpu_id = rank;
        graph.add_edge(root, prior, core::DagEdgeKind::Mutation);
        graph.add_edge(prior, result[rank], core::DagEdgeKind::Mutation);
    }
    return result;
}
void query_worker_check_and_drain() {
    auto config = timing_config();
    config.io_cost.storage_batch_pages = 2;
    config.io_cost.service_models.at("prefetch").stages = { 10, 0, 5, 0, 10, 0 };
    model::HiCacheState state(config);
    seed_storage(state, "rank:0:cache");
    seed_storage(state, "rank:1:cache", true);
    state.begin_formal_window(true);
    const auto facts = candidates(state, "read", 0, origin);
    model::HiCachePrefetchQueueExecution query(state, facts, queue_timing(), model::PrefetchQueueAction::Query);
    auto drain_timing = queue_timing();
    for (auto & [rank, cpu] : drain_timing.cpu) cpu = { 0, 1, 1 };
    model::HiCachePrefetchQueueExecution drain(state, facts, drain_timing, model::PrefetchQueueAction::Drain);
    model::PrefetchCheckTiming control;
    for (int rank : { 0, 1 }) {
        control.cpu[rank] = { 0, 0, 0, 0, 0, 0 };
        control.state_max.calls[rank] = {
            .entry_ranks = { 0, 1 },
            .worker_remainder = 4
        };
        control.completion_min.calls[rank] = {
            .entry_ranks = { 0, 1 },
            .worker_remainder = 3
        };
    }
    model::HiCachePrefetchCheckExecution check(state, facts, control, "wait_complete");
    model::HiCachePrefetchExecution worker(state, config);
    core::DagGraph graph;
    const auto root = graph.add_synthetic_node({ .name = "submitted candidates" });
    const auto query_entries = entries(graph, root, 0), check_entries = entries(graph, root, 35), drain_entries = entries(graph, root, 20);
    const auto response = graph.add_synthetic_node({ .name = "HTTP response", .counts_toward_e2e = true });
    graph.add_edge(root, response, core::DagEdgeKind::Mutation);
    const auto result = simulation::run_topological_simulation(graph, {}, [&](size_t node, uint64_t time, simulation::FutureDag & future) {
        if (node == root) {
            (void)query.start(query_entries, future);
            (void)drain.start(drain_entries, future);
            for (const auto [rank, returned] : check.start(check_entries, future)) future.depend(returned, response);
        }
        if (const auto rank = query.advance(node, origin + time, future)) {
            require(time == (*rank ? 16 : 14), "query must pay CPU work, late-peer wait and measured call remainder");
            require(worker.enqueue(facts.at(*rank), future).has_value(), "accepted query must launch its payload");
        }
        (void)drain.advance(node, origin + time, future);
        if (const auto ready = check.advance(node, origin + time, future)) require(*ready, "completed payload must permit foreground progress");
        worker.advance(node, origin + time, future);
    });
    require(result.e2e_us == 42, "nonzero query/control costs must propagate to foreground completion");
    for (const auto & [rank, fact] : facts) {
        const auto * op = state.prefetch_candidate_operation(fact);
        require(op->hit_pages.size() == 1 && op->completed_pages.size() == 1 && op->reserved_host_pages == 0,
                "asymmetric storage hits must be MINed before reading; unhit buffers must drain independently");
        require(op->io_return_ts == origin + (rank ? 41 : 39), "payload must start at its own query return");
    }
    hicache_timing_fixture::require_static_replay(graph, "generated query/control/worker graph must preserve every execution time on static replay");
}

void staggered_query_entries(int active_ranks) {
    auto config = timing_config();
    model::HiCacheState state(config);
    seed_storage(state, "rank:0:cache");
    seed_storage(state, "rank:1:cache");
    state.begin_formal_window(true);
    std::map<int, HiCacheFact> facts;
    for (int rank : { 0, 1 }) {
        auto fact = request_fact("prefetch_candidate_anchor", "staggered", origin + rank * 20, 0);
        fact.cache_scope = "rank:" + std::to_string(rank) + ":cache";
        // A short suffix is legitimately below the target's prefetch threshold.
        if (rank >= active_ranks) {
            fact.token_count = fact.full_path_span.end = fact.full_path_span.token_count = 1;
            fact.full_path_tokens.resize(1);
        }
        facts[rank] = fact;
    }
    model::HiCachePrefetchQueueExecution query(state, facts, queue_timing(), model::PrefetchQueueAction::Query);
    core::DagGraph graph;
    const auto root = graph.add_synthetic_node({ .name = "before candidates", .counts_toward_e2e = true });
    auto submitted = entries(graph, root, 0);
    submitted.at(1) = entries(graph, root, 20).at(1);
    CpuRankTimes returns;
    bool rejected = false;
    try {
        (void)simulation::run_topological_simulation(graph, {}, [&](size_t node, uint64_t time, simulation::FutureDag & future) {
            if (node == root) (void)query.start(submitted, future);
            for (const auto & [rank, entry] : submitted) {
                if (node != entry) continue;
                if (rank == 1 && active_ranks) {
                    const auto * first = state.prefetch_candidate_operation(facts.at(0));
                    require(first && first->query_sample_ts == origin + 4 && !first->query_return_ts,
                            "early rank must sample before the late candidate, then wait only at MIN");
                }
                state.apply_fact(facts.at(rank), HiCacheFactRole::PrefetchCandidateAnchor);
            }
            if (const auto rank = query.advance(node, origin + time, future)) returns[*rank] = time;
        });
    }
    catch (const std::logic_error & e) {
        rejected = std::string(e.what()) == "Prefetch query cannot mix absent candidates with collective participants";
        if (!rejected) throw;
    }
    if (active_ranks == 1) {
        require(rejected, "mixed candidate presence must not silently bypass the required MIN");
        return;
    }
    require(!rejected && returns.size() == 2, "both query entries must return");
    for (int rank : { 0, 1 }) {
        if (active_ranks) {
            const auto * op = state.prefetch_candidate_operation(facts.at(rank));
            require(op && op->query_sample_ts == origin + (rank ? 28 : 4), "query must sample at its own entry plus local CPU cost");
            require(returns.at(rank) == (rank ? 36 : 34), "query must preserve peer-entry wait and measured CPU/communication costs");
        }
        else require(returns.at(rank) == rank * 20, "absent candidates must return without query cost or a peer barrier");
    }
    if (!active_ranks)
        for (const auto & node : graph.nodes())
            require(graph.event_for_node(node.id).cat != "cpu_collective", "absent candidates must not generate a collective");
    hicache_timing_fixture::require_static_replay(graph, "lazy query graph must retain all node times on static replay");
}

void drain_uses_old_samples() {
    auto config = timing_config();
    model::HiCacheState state(config);
    seed_storage(state, "rank:0:cache");
    seed_storage(state, "rank:1:cache");
    state.begin_formal_window(true);
    const auto old = candidates(state, "old-miss", 100'000, origin);
    auto timing = queue_timing();
    for (auto & [rank, cpu] : timing.cpu) cpu = { 0, 0, 0 };
    model::HiCachePrefetchQueueExecution query(state, old, timing, model::PrefetchQueueAction::Query);
    model::HiCachePrefetchQueueExecution drain(state, old, timing, model::PrefetchQueueAction::Drain);
    core::DagGraph graph;
    const auto root = graph.add_synthetic_node({ .name = "candidates", .counts_toward_e2e = true });
    const auto query_entries = entries(graph, root, 0), drain_entries = entries(graph, root, 10), later_entries = entries(graph, root, 12);
    std::map<int, HiCacheFact> later;
    (void)simulation::run_topological_simulation(graph, {}, [&](size_t node, uint64_t time, simulation::FutureDag & future) {
        if (node == root) {
            (void)query.start(query_entries, future);
            (void)drain.start(drain_entries, future);
        }
        (void)query.advance(node, origin + time, future);
        (void)drain.advance(node, origin + time, future);
        if (node == later_entries.at(0)) {
            later = candidates(state, "new-miss", 200'000, origin + time);
            for (const auto & [rank, fact] : later) complete_query(state, fact);
        }
    });
    for (int rank : { 0, 1 }) {
        const auto * old_op = state.prefetch_candidate_operation(old.at(rank));
        const auto * new_op = state.prefetch_candidate_operation(later.at(rank));
        const auto queued = state.prefetch_queue_sizes(old.at(rank));
        require(old_op->reserved_host_pages == 0 && old_op->prefetch_state == runtime::HiCachePrefetchState::Revoked && new_op->reserved_host_pages == 2
                    && queued.released_pages == 2 && queued.revoked_operations == 1,
                "arrivals during drain MIN must stay queued for the next scheduler round");
    }
}

void backup_ack_keeps_host_pinned() {
    auto config = timing_config();
    config.l1_capacity_pages = config.l2_capacity_pages = 2;
    config.prefetch_capacity_limit_pages = 2; // Isolate host pins from the automatic L2-minus-L1 rate limit.
    model::HiCacheState state(config);
    state.begin_formal_window(true);
    auto backup = request_fact("cache_lifecycle_commit", "backup", origin, 0);
    backup.lifecycle_kind = "finished";
    state.apply_fact(backup, HiCacheFactRole::CacheLifecycleCommit);
    backup.ts = origin + 10;
    state.finalize();
    require(state.prefetch_queue_sizes(backup).backup_acks == 0, "reporting cannot force unfinished background writes into the ACK queue");
    backup.ts = origin + 20;
    complete_test_writes(state, backup);
    state.acknowledge_writes(backup, 1);
    backup.ts = origin + 100;
    const auto sampled = state.prefetch_queue_sizes(backup);
    require(sampled.backup_acks == 1, "completed storage service must enqueue one scheduler ACK");

    // Evict the device copy, leaving an otherwise evictable host leaf. Ordinary
    // model facts must not consume the storage ACK or release its host pin.
    auto extend = request_fact("cache_extend_input", "other-compute", origin + 101, 1'000);
    extend.is_start = true;
    extend.is_end = false;
    extend.batch_paths = {
        { .request_id = extend.request_id,
         .full_path_span = extend.full_path_span,
         .full_path_tokens = extend.full_path_tokens,
         .token_count = extend.token_count }
    };
    state.apply_fact(extend, HiCacheFactRole::CacheExtendInput);
    auto before = request_fact("prefetch_candidate_anchor", "before-ack", origin + 102, 2'000);
    state.apply_fact(before, HiCacheFactRole::PrefetchCandidateAnchor);
    const auto * rejected = state.prefetch_candidate_operation(before);
    require(!rejected || rejected->reserved_host_pages == 0, "undrained backup must protect host capacity after data is readable");
    require(state.prefetch_queue_sizes(before).backup_acks == 1, "ordinary requests do not drain storage ACKs");
    backup.ts = origin + 103;
    state.drain_prefetch_queues(backup, sampled);
    auto after = request_fact("prefetch_candidate_anchor", "after-ack", origin + 104, 3'000);
    state.apply_fact(after, HiCacheFactRole::PrefetchCandidateAnchor);
    const auto * admitted = state.prefetch_candidate_operation(after);
    require(admitted && admitted->reserved_host_pages == 2 && state.prefetch_queue_sizes(after).backup_acks == 0,
            "scheduler ACK must release exactly the host pin needed for subsequent admission");
}

void write_ack_is_not_dma_completion() {
    auto config = timing_config();
    config.l1_capacity_pages = 16;
    config.l2_capacity_pages = 32;
    model::HiCacheState state(config);
    state.begin_formal_window(true);
    std::map<int, HiCacheFact> facts;
    for (const int rank : { 0, 1 }) {
        auto fact = request_fact("cache_lifecycle_commit", "first", origin + rank * 10, 0);
        fact.cache_scope = "rank:" + std::to_string(rank) + ":cache";
        fact.lifecycle_kind = "finished";
        state.apply_fact(fact, HiCacheFactRole::CacheLifecycleCommit);
        facts[rank] = fact;
    }
    std::map<int, uint64_t> first_sample;
    for (auto [rank, fact] : facts) {
        fact.ts = origin + 25;
        complete_test_writes(state, fact);
        first_sample[rank] = state.write_completion_count(fact);
    }
    require(first_sample.at(0) == 1 && first_sample.at(1) == 0, "DMA prefix must use each rank's actual target readiness");
    for (auto [rank, fact] : facts) {
        fact.ts = origin + 30;
        auto second = request_fact("cache_lifecycle_commit", "second", fact.ts, 1'000);
        second.cache_scope = fact.cache_scope;
        second.lifecycle_kind = "finished";
        state.apply_fact(second, HiCacheFactRole::CacheLifecycleCommit);
        fact.ts = origin + 35;
        complete_test_writes(state, fact);
        state.acknowledge_writes(fact, std::min(first_sample.at(0), first_sample.at(1)));
        require(state.write_completion_count(fact) == 1 && !state.prefetch_queue_sizes(fact).backup_acks,
                "MIN zero must retain newly completed DMA; ordinary facts cannot acknowledge or submit storage");
    }
    const auto before = state.effect_decision_ledger();
    for (const auto & d : before.decisions)
        if (d.effect_type == model::HiCacheEffectType::CommitHostToStorage)
            require(!d.effective_page_count, "H2S cannot be submitted before the scheduler write ACK");
    for (auto [rank, fact] : facts) {
        fact.ts = origin + 36;
        const auto sampled = state.write_completion_count(fact);
        fact.ts = origin + 60; // The second DMA completes during this MIN.
        complete_test_writes(state, fact);
        require(state.write_completion_count(fact) == 2 && sampled == 1, "fixture must complete another batch during MIN");
        state.acknowledge_writes(fact, sampled);
        require(state.write_completion_count(fact) == 1 && !state.prefetch_queue_sizes(fact).backup_acks,
                "write ACK consumes only sampled prefix and starts, not completes, storage service");
        fact.ts = origin + 75;
        state.acknowledge_writes(fact, 1);
        require(!state.write_completion_count(fact), "the next check consumes the remaining DMA batch");
        fact.ts = origin + 79;
        require(!state.prefetch_queue_sizes(fact).backup_acks, "first H2S must start at write ACK 60, not DMA-ready 20");
        fact.ts = origin + 80;
        require(state.prefetch_queue_sizes(fact).backup_acks == 1, "first actual storage service completes at ACK plus 20");
        fact.ts = origin + 100;
        require(state.prefetch_queue_sizes(fact).backup_acks == 2, "second storage submission must queue behind the first");
    }
}

void pending_write_reserves_capacity_and_survives_split() {
    auto config = timing_config();
    config.l1_capacity_pages = config.l2_capacity_pages = 2;
    config.prefetch_capacity_limit_pages = 2;
    model::HiCacheState state(config);
    state.begin_formal_window(true);
    auto backup = request_fact("cache_lifecycle_commit", "backup", origin, 0);
    backup.lifecycle_kind = "finished";
    state.apply_fact(backup, HiCacheFactRole::CacheLifecycleCommit);
    auto split = request_fact("cache_lookup_input", "prefix-lookup", origin + 5, 0);
    split.token_count = split.full_path_span.end = split.full_path_span.token_count = 16;
    split.full_path_tokens.resize(16);
    state.apply_fact(split, HiCacheFactRole::CacheLookupInput);
    auto pressure = request_fact("prefetch_candidate_anchor", "pressure", origin + 6, 2'000);
    state.apply_fact(pressure, HiCacheFactRole::PrefetchCandidateAnchor);
    const auto * rejected = state.prefetch_candidate_operation(pressure);
    require(!rejected || !rejected->reserved_host_pages, "pending write allocation must occupy host space before its DMA ACK");
    backup.ts = origin + 7;
    state.apply_fact(backup, HiCacheFactRole::CacheLifecycleCommit);
    backup.ts = origin + 30;
    complete_test_writes(state, backup);
    require(state.write_completion_count(backup) == 1, "split pending host buffers must not be backed up again");
    state.acknowledge_writes(backup, 1);
    auto extend = request_fact("cache_extend_input", "evict-device", origin + 31, 3'000);
    extend.is_start = true;
    extend.is_end = false;
    extend.batch_paths = {
        { .request_id = extend.request_id,
         .full_path_span = extend.full_path_span,
         .full_path_tokens = extend.full_path_tokens,
         .token_count = extend.token_count }
    };
    state.apply_fact(extend, HiCacheFactRole::CacheExtendInput);
    auto finished = request_fact("cache_lifecycle_commit", "evict-device", origin + 32, 3'000);
    finished.lifecycle_kind = "finished";
    state.apply_fact(finished, HiCacheFactRole::CacheLifecycleCommit);
    auto read = request_fact("prefetch_candidate_anchor", "read-back", origin + 33, 0);
    state.apply_fact(read, HiCacheFactRole::PrefetchCandidateAnchor);
    read.role = "cache_lookup_input";
    read.ts = origin + 34;
    read.source_node_id++;
    state.apply_fact(read, HiCacheFactRole::CacheLookupInput);
    bool loaded = false;
    for (const auto & d : state.effect_decision_ledger().decisions)
        if (d.effect_type == model::HiCacheEffectType::Loadback && d.source_node_id == read.source_node_id) loaded = d.effective_page_count == 2;
    require(loaded, "DMA ACK must publish every split fragment of the original host buffer, before storage completion");
}

void backup_ack_min_preserves_late_completions() {
    auto config = timing_config();
    config.l1_capacity_pages = 16;
    config.l2_capacity_pages = 32;
    model::HiCacheState state(config);
    state.begin_formal_window(true);
    core::DagGraph graph;
    const auto root = graph.add_synthetic_node({ .name = "before backups", .counts_toward_e2e = true });
    std::map<size_t, HiCacheFact> commits;
    std::map<size_t, HiCacheFact> write_acks;
    std::map<int, HiCacheFact> scopes;
    for (int rank : { 0, 1 }) {
        auto fact = request_fact("cache_lifecycle_commit", "first", origin + 10 * rank, 0);
        fact.cache_scope = "rank:" + std::to_string(rank) + ":cache";
        fact.lifecycle_kind = "finished";
        scopes[rank] = fact;
        for (const auto at : { uint64_t(10 * rank), uint64_t(30) }) {
            if (at == 30) fact = request_fact("cache_lifecycle_commit", "second", origin + at, 1'000);
            fact.cache_scope = scopes.at(rank).cache_scope;
            fact.lifecycle_kind = "finished";
            const auto before = graph.add_synthetic_node({ .name = "work before backup", .duration = at });
            const auto commit = graph.add_synthetic_node({ .name = "backup submission" });
            graph.add_edge(root, before, core::DagEdgeKind::Mutation);
            graph.add_edge(before, commit, core::DagEdgeKind::Mutation);
            commits[commit] = fact;
        }
        // Explicit fixture MIN returns: the first common DMA batch is
        // acknowledged at 40/50; both ranks acknowledge the second at 70.
        for (const uint64_t at : { uint64_t(40 + 10 * rank), uint64_t(70) }) {
            const auto delay = graph.add_synthetic_node({ .name = "before write acknowledgement", .duration = at });
            const auto ack = graph.add_synthetic_node({ .name = "write MIN returned one" });
            graph.add_edge(root, delay, core::DagEdgeKind::Mutation);
            graph.add_edge(delay, ack, core::DagEdgeKind::Mutation);
            auto ready = scopes.at(rank);
            ready.ts = origin + at;
            write_acks[ack] = ready;
        }
    }
    auto timing = queue_timing();
    for (int rank : { 0, 1 }) {
        timing.cpu[rank] = { 0, 0, 0 };
        timing.agreement.calls.at(rank).envelope_remainder_us = 8 + 2 * rank;
    }
    std::vector<CpuRankNodes> gates;
    std::vector<std::unique_ptr<model::HiCachePrefetchQueueExecution>> drains;
    for (const auto at : { 65, 85, 110 }) {
        gates.push_back(entries(graph, root, at));
        drains.push_back(std::make_unique<model::HiCachePrefetchQueueExecution>(state, scopes, timing, model::PrefetchQueueAction::Drain));
    }
    size_t returns = 0;
    (void)simulation::run_topological_simulation(graph, {}, [&](size_t node, uint64_t time, simulation::FutureDag & future) {
        if (node == root)
            for (size_t i = 0; i < drains.size(); ++i) (void)drains[i]->start(gates[i], future);
        if (const auto found = commits.find(node); found != commits.end()) state.apply_fact(found->second, HiCacheFactRole::CacheLifecycleCommit);
        if (const auto found = write_acks.find(node); found != write_acks.end()) {
            complete_test_writes(state, found->second);
            state.acknowledge_writes(found->second, 1);
        }
        for (size_t i = 0; i < drains.size(); ++i) {
            const auto rank = drains[i]->advance(node, origin + time, future);
            if (!rank) continue;
            auto at = scopes.at(*rank);
            at.ts = origin + time;
            const auto count = state.prefetch_queue_sizes(at).backup_acks;
            require(count == (i == 2 ? 0 : 1), "MIN only drains the previously sampled common prefix, not ACKs arriving during communication");
            ++returns;
        }
    });
    require(returns == 6, "all three backup-drain rounds must return on both ranks");
    hicache_timing_fixture::require_static_replay(graph, "backup ACK control costs must remain in the generated DAG");
}

void replaced_pending_query_keeps_identity() {
    auto config = timing_config();
    config.io_cost.storage_batch_pages = 2;
    config.io_cost.service_models.at("prefetch").stages = { 10, 0, 5, 0, 10, 0 };
    model::HiCacheState state(config);
    seed_storage(state);
    state.begin_formal_window(true);
    const auto old = request_fact("prefetch_candidate_anchor", "same", origin, 0);
    state.apply_fact(old, HiCacheFactRole::PrefetchCandidateAnchor);
    auto replacement = old;
    replacement.source_node_id++;
    replacement.ts += 2;
    model::PrefetchQueueTiming timing;
    timing.cpu[0] = { 4, 2, 1 }; // Single-rank query has no communication.
    model::HiCachePrefetchQueueExecution query(state,
                                               {
                                                   { 0, old }
    },
                                               timing,
                                               model::PrefetchQueueAction::Query);
    model::HiCachePrefetchExecution worker(state, config);
    core::DagGraph graph;
    const auto root = graph.add_synthetic_node({ .name = "old queued query", .counts_toward_e2e = true });
    const auto stop_delay = graph.add_synthetic_node({ .name = "before stop", .duration = 1 });
    const auto stop_at = graph.add_synthetic_node({ .name = "stop" });
    const auto replace_delay = graph.add_synthetic_node({ .name = "before replacement", .duration = 2 });
    const auto replace_at = graph.add_synthetic_node({ .name = "replacement" });
    graph.add_edge(root, stop_delay, core::DagEdgeKind::Mutation);
    graph.add_edge(stop_delay, stop_at, core::DagEdgeKind::Mutation);
    graph.add_edge(root, replace_delay, core::DagEdgeKind::Mutation);
    graph.add_edge(replace_delay, replace_at, core::DagEdgeKind::Mutation);
    (void)simulation::run_topological_simulation(graph, {}, [&](size_t node, uint64_t time, simulation::FutureDag & future) {
        if (node == root)
            (void)query.start(
                {
                    { 0, root }
            },
                future);
        if (node == stop_at) {
            auto stop = old;
            stop.ts = origin + time;
            (void)state.stop_prefetch(stop);
            state.publish_prefetch(stop, 0);
        }
        if (node == replace_at) state.apply_fact(replacement, HiCacheFactRole::PrefetchCandidateAnchor);
        if (query.advance(node, origin + time, future)) (void)worker.enqueue(old, future);
        worker.advance(node, origin + time, future);
    });
    const auto * old_op = state.prefetch_candidate_operation(old);
    const auto * new_op = state.prefetch_candidate_operation(replacement);
    require(old_op != new_op && old_op->query_return_ts == origin + 7 && old_op->io_return_ts == origin + 42 && !new_op->query_sample_ts
                && !new_op->payload_transfer_issued,
            "late old query must not sample, submit or complete the replacement request operation");
}
} // namespace

void check_prefetch_queue_execution() {
    for (int active_ranks : { 0, 1, 2 }) staggered_query_entries(active_ranks);
    pending_write_reserves_capacity_and_survives_split();
    write_ack_is_not_dma_completion();
    backup_ack_keeps_host_pinned();
    backup_ack_min_preserves_late_completions();
    query_worker_check_and_drain();
    drain_uses_old_samples();
    replaced_pending_query_keeps_identity();
}
