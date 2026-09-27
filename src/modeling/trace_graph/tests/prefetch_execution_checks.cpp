#include "hicache_timing_fixture.hpp"
#include "markov/trace_graph/modules/hicache/missing_cost.hpp"
#include "markov/trace_graph/modules/hicache/model/prefetch_execution.hpp"
#include "markov/trace_graph/modules/hicache/patch/io_resource_model.hpp"
#include "markov/trace_graph/modules/hicache/prefetch_control.hpp"
#include <algorithm>
#include <stdexcept>
using namespace hicache_timing_fixture;

namespace {
void measured_stages_drive_publications() {
    frontend::HiCacheIoServiceModelConfig service;
    service.direction = "storage_to_host";
    require(!hicache_prefetch_batch(service, 10, 8, 0), "aggregate service does not invent publication times");
    service.stages = { 2, .1, 3, .2, 5, .5 };
    service.runtime_scale = 2;

    for (const uint64_t origin : { 100ULL, 1'000'000'100ULL }) {
        const auto batch = hicache_prefetch_batch(service, 10, 8, origin);
        require(batch && batch->publications.size() == 8, "calibrated service publishes every page");
        require(batch->publications.front().at_us == origin + 58 && batch->publications.back().at_us == origin + 128,
                "page publication follows allocation/read and precedes cleanup");
        require(batch->ready_ts == origin + hicache_service_cost(service, 10, 8)->duration_us && batch->ready_ts == origin + 146,
                "aggregate and staged service must charge the same executed work");

        for (const auto stop : { origin, origin + 58, origin + 60, origin + 128, origin + 129, origin + 146 }) {
            const auto executed = execute_prefetch_batch(*batch, stop);
            const auto cost = hicache_service_cost(service, 10, 8, 1, 0, executed.copied_pages);
            require(cost && origin + cost->duration_us == executed.release_us, "cancelled service pays the actual copies and cleanup");
            require(executed.published_pages + static_cast<uint64_t>(executed.cancelled) == executed.copied_pages,
                    "only the rejected final copy is not published");
        }
    }

    require(!hicache_service_cost(service, 10, 8, 1, 0, 9), "copied pages cannot exceed read pages");
    require(!hicache_service_cost(service, 10, 8, 1, 0, 0), "issued generic batch always attempts a first copy");
}

void physical_worker_runs_on_the_dag() {
    struct Case {
        uint64_t query, stop, batch, returned, read, copied, published;
    };
    for (const auto c : std::vector<Case>{
             {  0, 100, 2, 40, 2, 2, 2 },
             { 10,   5, 2, 45, 2, 1, 0 },
             {  0,  22, 2, 35, 2, 1, 0 },
             {  0,  25, 2, 35, 2, 1, 0 },
             {  0,  27, 2, 40, 2, 2, 1 },
             {  0,  30, 2, 40, 2, 2, 1 },
             {  0,  31, 2, 40, 2, 2, 2 },
             {  0,  15, 1, 25, 1, 1, 0 },
             {  0,  20, 1, 50, 2, 2, 1 },
             {  0,  40, 1, 50, 2, 2, 1 },
             {  0, 100, 1, 50, 2, 2, 2 }
    }) {
        auto config = timing_config();
        config.io_cost.storage_batch_pages = c.batch;
        config.io_cost.service_models.at("prefetch").stages = { 10, 0, 5, 0, 10, 0 };
        model::HiCacheState state(config);
        seed_storage(state);
        state.begin_formal_window(true);
        model::HiCachePrefetchExecution worker(state, config);
        const uint64_t origin = 200'000;
        auto candidate = request_fact("prefetch_candidate_anchor", "read", origin, 0);
        core::DagGraph graph;
        const auto begin = graph.add_synthetic_node({ .name = "candidate" });
        const auto query_work = graph.add_synthetic_node({ .name = "query work", .duration = c.query });
        const auto query = graph.add_synthetic_node({ .name = "query MIN returned" });
        const auto foreground = graph.add_synthetic_node({ .name = "foreground", .duration = c.stop });
        const auto stop_entry = graph.add_synthetic_node({ .name = "stop entry" });
        const auto response = graph.add_synthetic_node({ .name = "HTTP response", .duration = 8, .counts_toward_e2e = true });
        for (const auto next : { query_work, foreground, response }) graph.add_edge(begin, next, core::DagEdgeKind::Mutation);
        graph.add_edge(query_work, query, core::DagEdgeKind::Mutation);
        graph.add_edge(foreground, stop_entry, core::DagEdgeKind::Mutation);
        size_t stop = core::DagNode::kNoNode;
        std::optional<size_t> returned;
        uint64_t published = 0;
        const auto replay = simulation::run_topological_simulation(graph, {}, [&](size_t id, uint64_t time, simulation::FutureDag & future) {
            worker.advance(id, origin + time, future);
            auto fact = candidate;
            fact.ts = origin + time;
            if (id == begin) state.apply_fact(fact, HiCacheFactRole::PrefetchCandidateAnchor);
            if (id == query) {
                complete_query(state, fact);
                returned = worker.enqueue(fact, future);
            }
            // Deliberately created later than a simultaneous publication node:
            // cancellation priority must not depend on numerical node identity.
            if (id == stop_entry) stop = future.append({ .name = "actual stop" });
            if (id == stop) published = state.stop_prefetch(fact);
        });
        const auto * op = state.prefetch_operation(candidate);
        uint64_t read = 0, copied = 0;
        for (const auto & batch : op->io_schedule.batches) {
            read += batch.page_count;
            copied += *batch.copied_page_count;
        }
        if (!returned || graph.node(*returned).completion_time != c.returned || op->io_return_ts != origin + c.returned || read != c.read || copied != c.copied
            || published != c.published || replay.e2e_us != 8)
            throw std::runtime_error("dynamic worker must preserve read/copy/cancel/cleanup work without turning background return into HTTP");

        const auto static_result =
            hicache_timing_fixture::require_static_replay(graph, "generated physical worker times must survive replay without the state callback");
        if (static_result.e2e_us != 8) throw std::runtime_error("background work changed static HTTP replay");
    }
}

void queued_workers_follow_actual_release() {
    for (const bool shared : { false, true }) {
        auto config = timing_config();
        config.io_cost.storage_batch_pages = 2;
        config.io_cost.resource_lanes.shared_storage_read = shared;
        config.io_cost.service_models.at("prefetch").stages = { 10, 0, 5, 0, 10, 0 };
        model::HiCacheState state(config);
        seed_storage(state, "rank:0:cache");
        seed_storage(state, "rank:1:cache");
        state.begin_formal_window(true);
        model::HiCachePrefetchExecution worker(state, config);
        const uint64_t origin = 200'000;
        auto first = request_fact("prefetch_candidate_anchor", "first", origin, 0), next = first;
        first.cache_scope = "rank:0:cache";
        next.cache_scope = "rank:1:cache";
        next.request_id = "next";
        next.source_node_id++;
        core::DagGraph graph;
        const auto begin = graph.add_synthetic_node({ .name = "queries" });
        const auto prior = graph.add_synthetic_node({ .name = "before stop first", .duration = 22 });
        const auto queued = graph.add_synthetic_node({ .name = "before stop next", .duration = 15 });
        const auto stop_first = graph.add_synthetic_node({ .name = "stop first", .counts_toward_e2e = true });
        const auto stop_next = graph.add_synthetic_node({ .name = "stop next" });
        graph.add_edge(begin, prior, core::DagEdgeKind::Mutation);
        graph.add_edge(prior, stop_first, core::DagEdgeKind::Mutation);
        graph.add_edge(begin, queued, core::DagEdgeKind::Mutation);
        graph.add_edge(queued, stop_next, core::DagEdgeKind::Mutation);
        std::optional<size_t> returned_first, returned_next;
        (void)simulation::run_topological_simulation(graph, {}, [&](size_t id, uint64_t time, simulation::FutureDag & future) {
            worker.advance(id, origin + time, future);
            if (id == begin) {
                for (auto fact : { first, next }) {
                    state.apply_fact(fact, HiCacheFactRole::PrefetchCandidateAnchor);
                    complete_query(state, fact);
                }
                returned_first = worker.enqueue(first, future);
                returned_next = worker.enqueue(next, future);
            }
            auto fact = id == stop_first ? first : next;
            fact.ts = origin + time;
            if (id == stop_first || id == stop_next) (void)state.stop_prefetch(fact);
        });
        if (!returned_first || !returned_next || graph.node(*returned_first).completion_time != 35
            || graph.node(*returned_next).completion_time != (shared ? 70 : 35))
            throw std::runtime_error("queued cancellation must follow actual resource return; independent lanes must remain independent");
        for (auto fact : { first, next }) {
            fact.ts = origin + 80;
            if (state.prefetch_queue_sizes(fact).released_pages != 2) throw std::runtime_error("worker return must queue its unpublished pages once");
            state.drain_prefetch_queues(fact, state.prefetch_queue_sizes(fact));
            if (state.prefetch_operation(fact)->reserved_host_pages) throw std::runtime_error("drain must reclaim each worker's reservation");
        }
    }
}

void replaced_request_keeps_its_background_identity() {
    auto config = timing_config();
    config.io_cost.storage_batch_pages = 2;
    config.io_cost.service_models.at("prefetch").stages = { 10, 0, 5, 0, 10, 0 };
    model::HiCacheState state(config);
    seed_storage(state);
    state.begin_formal_window(true);
    model::HiCachePrefetchExecution worker(state, config);
    const uint64_t origin = 200'000;
    auto candidate = request_fact("prefetch_candidate_anchor", "same-request", origin, 0);
    const runtime::HiCachePrefetchOperation * old = nullptr;
    core::DagGraph graph;
    const auto begin = graph.add_synthetic_node({ .name = "candidate" });
    const auto prior = graph.add_synthetic_node({ .name = "before replacement", .duration = 22 });
    const auto replace = graph.add_synthetic_node({ .name = "replace", .counts_toward_e2e = true });
    graph.add_edge(begin, prior, core::DagEdgeKind::Mutation);
    graph.add_edge(prior, replace, core::DagEdgeKind::Mutation);
    (void)simulation::run_topological_simulation(graph, {}, [&](size_t id, uint64_t time, simulation::FutureDag & future) {
        worker.advance(id, origin + time, future);
        auto fact = candidate;
        fact.ts = origin + time;
        if (id == begin) {
            state.apply_fact(fact, HiCacheFactRole::PrefetchCandidateAnchor);
            complete_query(state, fact);
            old = state.prefetch_operation(fact);
            (void)worker.enqueue(fact, future);
        }
        if (id == replace) {
            (void)state.stop_prefetch(fact);
            state.publish_prefetch(fact, 0);
            ++fact.source_node_id;
            state.apply_fact(fact, HiCacheFactRole::PrefetchCandidateAnchor);
            // Its query is deliberately still pending when the old I/O returns.
            if (worker.enqueue(fact, future)) throw std::runtime_error("pending query cannot create payload work");
        }
    });
    const auto * latest = state.prefetch_operation(candidate);
    if (!old || latest == old || old->io_return_ts != origin + 35 || latest->io_return_ts || latest->reserved_host_pages != 2)
        throw std::runtime_error("old background return must not complete the new request operation");
    candidate.ts = origin + 40;
    state.drain_prefetch_queues(candidate, state.prefetch_queue_sizes(candidate));
    if (old->reserved_host_pages || latest->reserved_host_pages != 2)
        throw std::runtime_error("old worker release must preserve the replacement operation's buffers");
}

void executed_prefetch_changes_state_and_cost() {
    auto config = timing_config();
    config.io_cost.storage_batch_pages = 2;
    config.prefetch_policy = "timeout";
    config.prefetch_timeout_configured = true;
    config.prefetch_timeout_base_sec = config.prefetch_timeout_max_sec = .000022;
    config.io_cost.service_models.at("prefetch").stages = { 10, 0, 5, 0, 10, 0 };
    const uint64_t origin = 200'000;
    for (const uint64_t first_check : { 23, 35, 23 }) {
        model::HiCacheState state(config);
        seed_storage(state);
        state.begin_formal_window(/*execution_prefetch_control=*/true);
        auto candidate = request_fact("prefetch_candidate_anchor", "read", origin, 0);
        state.apply_fact(candidate, HiCacheFactRole::PrefetchCandidateAnchor);
        complete_query(state, candidate);
        const auto * op = state.prefetch_operation(candidate);
        if (!op || op->io_schedule.ready_ts != origin + 40 || op->io_schedule.batches[0].page_ready_ts.size() != 2)
            throw std::runtime_error("execution-driven enqueue must not truncate publications at the policy deadline");

        core::DagGraph graph;
        const auto launch = graph.add_synthetic_node({ .name = "query MIN returned" });
        model::HiCachePrefetchExecution worker(state, config);
        std::optional<size_t> worker_return;
        std::vector<size_t> nodes(7, core::DagNode::kNoNode);
        nodes[0] = graph.add_synthetic_node({ .name = "preceding work", .duration = first_check });
        nodes[6] = graph.add_synthetic_node({ .name = "extend", .counts_toward_e2e = true });
        graph.add_edge(nodes[0], nodes[6], core::DagEdgeKind::Mutation);
        graph.add_edge(launch, nodes[0], core::DagEdgeKind::Mutation);
        // Both checks occur after the timeout. Each single-rank collective
        // takes 8 us; both pages publish before stop at 31 or 43 us.
        const uint64_t collective_us = 8;
        std::optional<model::HiCacheLoadbackBatch> load_batch;
        const auto replay = simulation::run_topological_simulation(
            graph,
            [&](size_t id, uint64_t at, uint64_t duration) {
                auto boundary = request_fact("prefetch_progress_observed", "read", origin + at, 0);
                if (id == nodes[1]) return collective_us;
                if (id == nodes[2]) {
                    if (state.stop_prefetch(boundary) != 2 || !op->completed_pages.empty())
                        throw std::runtime_error("stop must record local physical progress without publishing shared cache state");
                }
                if (id == nodes[3]) return collective_us;
                if (id == nodes[4]) state.publish_prefetch(boundary, 2);
                if (id == nodes[5]) {
                    boundary.role = "cache_lookup_input";
                    state.apply_fact(boundary, HiCacheFactRole::CacheLookupInput);
                    const auto ledger = state.effect_decision_ledger();
                    const auto load =
                        std::ranges::find_if(ledger.decisions, [](const auto & item) { return item.effect_type == model::HiCacheEffectType::Loadback; });
                    if (load == ledger.decisions.end() || load->effective_page_count != 2)
                        throw std::runtime_error("executed MIN must expose the completed prefix to canonical loadback");
                    load_batch = state.submit_loadbacks(boundary);
                    if (!load_batch) throw std::runtime_error("load intent must be submitted before device execution");
                    return load_batch->io_schedule.ready_ts - boundary.ts;
                }
                if (id == nodes[6]) {
                    state.complete_loadback_batch(boundary, load_batch->id);
                    boundary.role = "cache_extend_input";
                    boundary.is_start = true;
                    boundary.is_end = false;
                    boundary.token_count = boundary.full_path_span.end = boundary.full_path_span.token_count = 48;
                    for (uint32_t token = 32; token < 48; ++token) boundary.full_path_tokens.push_back({ { token } });
                    boundary.batch_paths = {
                        { .request_id = "read", .full_path_span = boundary.full_path_span, .full_path_tokens = boundary.full_path_tokens, .token_count = 48 }
                    };
                    state.apply_fact(boundary, HiCacheFactRole::CacheExtendInput);
                    const auto & work = state.prefill_work_items();
                    if (work.empty() || work.back().prefill_token_count != 16)
                        throw std::runtime_error("visible cache must reduce subsequent canonical Prefill work");
                    return 2 * work.back().prefill_token_count;
                }
                return duration;
            },
            [&](size_t id, uint64_t time, simulation::FutureDag & future) {
                worker.advance(id, origin + time, future);
                if (id == launch) worker_return = worker.enqueue(candidate, future);
                if (id != nodes[0]) return;
                size_t i = 1;
                for (const auto * name : { "check/MAX", "stop", "completion MIN", "visible", "lookup" }) {
                    nodes[i] = future.append({ .name = name });
                    if (i > 1) future.depend(nodes[i - 1], nodes[i]);
                    ++i;
                }
                future.depend(nodes[5], nodes[6]);
            });
        if (replay.e2e_us != first_check + 8 + 8 + 20 + 32 || op->completed_pages.size() != 2)
            throw std::runtime_error("MAX-time publication, MIN visibility, loadback and compute must share the execution clock");
        if (!worker_return || graph.node(*worker_return).completion_time != 40 || op->io_return_ts != origin + 40)
            throw std::runtime_error("generated background work must actually return and release on the physical clock");
        if (simulation::run_topological_simulation(graph).e2e_us != replay.e2e_us)
            throw std::runtime_error("generated HiCache work must remain in the replayable DAG");
        const auto costs = patch::build_hicache_io_resource_plan(state.effect_decision_ledger(), config.io_cost);
        const auto cost = std::ranges::find_if(costs.costs, [](const auto & item) { return item.effect_type == model::HiCacheEffectType::PrefetchIo; });
        if (cost == costs.costs.end() || cost->duration_us != 40 || cost->storage_service_batches[0].copied_page_count != 2)
            throw std::runtime_error("DAG service cost must follow actual stop, not the earlier timeout deadline");
    }

    model::HiCacheState state(config);
    seed_storage(state);
    state.begin_formal_window(/*execution_prefetch_control=*/true);
    auto first = request_fact("prefetch_candidate_anchor", "first", origin, 0);
    state.apply_fact(first, HiCacheFactRole::PrefetchCandidateAnchor);
    complete_query(state, first);
    auto next = request_fact("prefetch_candidate_anchor", "next", origin + 10, 0);
    next.token_count = next.full_path_span.end = next.full_path_span.token_count = 16;
    next.full_path_tokens.resize(16);
    state.apply_fact(next, HiCacheFactRole::PrefetchCandidateAnchor);
    complete_query(state, next);
    if (!state.prefetch_operation(next) || state.prefetch_operation(next)->io_schedule.start_ts != origin + 40)
        throw std::runtime_error("next read must initially queue behind the full first service");
    first.ts = origin + 22;
    if (state.stop_prefetch(first) != 0 || state.prefetch_operation(next)->io_schedule.start_ts != origin + 35)
        throw std::runtime_error("actual cancellation must advance the queued read after the rejected copy and cleanup");
    first.ts = origin + 30;
    state.publish_prefetch(first, 0);
    next.ts = origin + 52;
    if (state.stop_prefetch(next) != 1) throw std::runtime_error("advanced queued work must publish before its actual stop");
    next.ts = origin + 60;
    state.publish_prefetch(next, 1);
    const auto executed = state.effect_decision_ledger();
    const auto executed_costs = patch::build_hicache_io_resource_plan(executed, config.io_cost);
    for (const auto & decision : executed.decisions) {
        if (decision.effect_type != model::HiCacheEffectType::PrefetchIo) continue;
        const auto cost = std::ranges::find_if(executed_costs.costs, [&](const auto & item) { return item.effect_id == decision.effect_key; });
        const uint64_t expected = decision.request_id_provenance == "first" ? 35 : 25;
        if (cost == executed_costs.costs.end() || cost->duration_us != expected || cost->storage_service_batches[0].copied_page_count != 1)
            throw std::runtime_error("cancelled and requeued physical execution must reach the DAG service cost without full-copy fallback");
    }
    state.finalize();
    if (state.prefetch_operation(first)->reserved_host_pages != 2)
        throw std::runtime_error("visibility or end-of-trace cannot replace the scheduler's host release queue");

    // A queued cancellation remains cancelled when an earlier operation later
    // shortens its service. It still reads one whole batch and rejects one copy.
    model::HiCacheState queued(config);
    seed_storage(queued);
    queued.begin_formal_window(true);
    first = request_fact("prefetch_candidate_anchor", "first", origin, 0);
    next = request_fact("prefetch_candidate_anchor", "next", origin + 10, 0);
    queued.apply_fact(first, HiCacheFactRole::PrefetchCandidateAnchor);
    queued.apply_fact(next, HiCacheFactRole::PrefetchCandidateAnchor);
    complete_query(queued, first);
    complete_query(queued, next);
    next.ts = origin + 15;
    if (queued.stop_prefetch(next) != 0) throw std::runtime_error("queued cancellation cannot publish before any read");
    first.ts = origin + 22;
    (void)queued.stop_prefetch(first);
    const auto * cancelled = queued.prefetch_operation(next);
    if (cancelled->io_schedule.start_ts != origin + 35 || cancelled->io_schedule.ready_ts != origin + 70
        || cancelled->io_schedule.batches[0].copied_page_count != 1 || !cancelled->io_schedule.batches[0].page_ready_ts.empty())
        throw std::runtime_error("rescheduling must retain queued stop time, full first read and rejected copy");

    for (const bool shared : { false, true }) {
        config.io_cost.resource_lanes.shared_storage_read = shared;
        model::HiCacheState ranks(config);
        seed_storage(ranks, "rank:0:cache");
        seed_storage(ranks, "rank:1:cache");
        ranks.begin_formal_window(true);
        auto a = request_fact("prefetch_candidate_anchor", "read", origin, 0);
        auto b = a;
        a.cache_scope = "rank:0:cache";
        b.cache_scope = "rank:1:cache";
        ranks.apply_fact(a, HiCacheFactRole::PrefetchCandidateAnchor);
        ranks.apply_fact(b, HiCacheFactRole::PrefetchCandidateAnchor);
        complete_query(ranks, a);
        complete_query(ranks, b);
        a.ts = origin + 22;
        const auto local_a = ranks.stop_prefetch(a);
        if (ranks.prefetch_operation(b)->io_schedule.start_ts != origin + (shared ? 35 : 0))
            throw std::runtime_error("cancellation must requeue only operations sharing the physical read resource");
        b.ts = origin + 27;
        const auto local_b = ranks.stop_prefetch(b);
        if (local_a != 0 || local_b != (shared ? 0 : 1)) throw std::runtime_error("per-rank progress must follow its own service start");
        a.ts = b.ts = origin + 35;
        const auto visible = std::min(local_a, local_b);
        ranks.publish_prefetch(a, visible);
        ranks.publish_prefetch(b, visible);
        if (!ranks.prefetch_operation(a)->completed_pages.empty() || !ranks.prefetch_operation(b)->completed_pages.empty()
            || ranks.prefetch_operation(b)->reserved_host_pages != 2)
            throw std::runtime_error("cross-rank MIN cannot expose or silently free another rank's excess pages");
    }
}

void prefetch_releases_follow_scheduler() {
    auto config = timing_config();
    config.io_cost.storage_batch_pages = 2;
    config.prefetch_policy = "timeout";
    config.prefetch_timeout_configured = true;
    config.prefetch_timeout_base_sec = config.prefetch_timeout_max_sec = .000022;
    config.io_cost.service_models.at("prefetch").stages = { 10, 0, 5, 0, 10, 0 };
    const uint64_t origin = 200'000;
    const auto require = [](bool ok, const char * message) {
        if (!ok) throw std::runtime_error(message);
    };
    model::HiCacheState state(config);
    seed_storage(state);
    state.begin_formal_window(true);
    auto old = request_fact("prefetch_candidate_anchor", "old", origin, 0);
    old.token_count = old.full_path_span.end = old.full_path_span.token_count = 48;
    for (uint32_t token = 32; token < 48; ++token) old.full_path_tokens.push_back({ { token } });
    state.apply_fact(old, HiCacheFactRole::PrefetchCandidateAnchor);
    require(!state.prefetch_operation(old)->payload_transfer_issued, "candidate cannot start physical reads before query return");
    old.ts = origin + 10;
    complete_query(state, old);
    require(state.prefetch_operation(old)->hit_pages.size() == 2 && state.prefetch_operation(old)->io_schedule.start_ts == origin + 10,
            "query-time storage prefix and actual query return determine issued work");
    const auto sampled = state.prefetch_queue_sizes(old);
    require(sampled.released_pages == 1 && sampled.revoked_operations == 0, "query queues only the unhit suffix");
    old.ts = origin + 22;
    require(state.stop_prefetch(old) == 0, "delayed query leaves no published page at stop");
    old.ts = origin + 30;
    state.publish_prefetch(old, 0);
    auto later = request_fact("prefetch_candidate_anchor", "later", origin + 32, 0);
    state.apply_fact(later, HiCacheFactRole::PrefetchCandidateAnchor);
    const auto later_reserved = state.prefetch_operation(later)->reserved_host_pages;
    require(state.prefetch_operation(later)->host_reserved_pages_at_enqueue == 3, "a new request must still see the old request's unreturned reservation");
    old.ts = origin + 44;
    hicache_timing_fixture::require_throws<std::logic_error>([&] { state.complete_prefetch_io(old); },
                                                             "return event cannot precede the rejected copy and physical cleanup");
    old.ts = origin + 46; // Explicit one-us worker tail after the 45-us physical return.
    state.complete_prefetch_io(old);
    require(state.prefetch_queue_sizes(old).released_pages == 3, "query and I/O enqueue disjoint release ranges");
    later.ts = origin + 47;
    state.drain_prefetch_queues(later, sampled);
    require(state.prefetch_operation(old)->reserved_host_pages == 2 && later_reserved > 0
                && state.prefetch_operation(later)->reserved_host_pages == later_reserved,
            "old sampled count releases old pages, not current request buffers (which may have been truncated at allocation)");
    state.drain_prefetch_queues(later, { .released_pages = 1 });
    require(state.prefetch_operation(old)->reserved_host_pages == 1, "page FIFO supports a partial operation drain");
    state.drain_prefetch_queues(later, { .released_pages = 1 });
    require(state.prefetch_operation(old)->reserved_host_pages == 0 && state.prefetch_queue_sizes(old).released_pages == 0,
            "each allocated page must be converted to residency or released exactly once");
    auto recovered = request_fact("prefetch_candidate_anchor", "after_release", origin + 50, 0);
    state.apply_fact(recovered, HiCacheFactRole::PrefetchCandidateAnchor);
    require(state.prefetch_operation(recovered)->requested_host_pages == 2
                && state.prefetch_operation(recovered)->host_reserved_pages_at_enqueue == later_reserved,
            "scheduler release must restore canonical capacity for later allocations, not only clear an operation counter");

    model::HiCacheState stopped(config);
    seed_storage(stopped);
    stopped.begin_formal_window(true);
    auto before_query = request_fact("prefetch_candidate_anchor", "early", origin, 0);
    stopped.apply_fact(before_query, HiCacheFactRole::PrefetchCandidateAnchor);
    before_query.ts = origin + 5;
    require(stopped.stop_prefetch(before_query) == 0, "stopping a query-pending operation is a valid branch");

    // A query-pending cancellation has no I/O work yet. Ask for a service
    // measurement only if the later query actually issues its first batch.
    auto missing_config = config;
    missing_config.io_cost.service_models.erase("prefetch");
    model::HiCacheState unpriced(missing_config);
    seed_storage(unpriced);
    unpriced.begin_formal_window(true);
    auto pending_query = request_fact("prefetch_candidate_anchor", "early", origin, 0);
    unpriced.apply_fact(pending_query, HiCacheFactRole::PrefetchCandidateAnchor);
    pending_query.ts = origin + 5;
    require(unpriced.stop_prefetch(pending_query) == 0, "no payload must not request an absent service model");
    pending_query.ts = origin + 10;
    try {
        complete_query(unpriced, pending_query);
        throw std::runtime_error("issued prefetch must not substitute zero for missing service");
    }
    catch (const MissingCostEvidence & error) {
        require(error.requirement.at("component") == "physical/prefetch_stages" && error.requirement.at("coordinates").at("page_count") > 0,
                "actual transfer must report the shared service and positive work");
    }

    before_query.ts = origin + 8;
    stopped.publish_prefetch(before_query, 0);
    require(!stopped.prefetch_operation(before_query)->timed_out, "an early explicit stop is not a policy timeout");
    before_query.ts = origin + 10;
    complete_query(stopped, before_query);
    const auto * pending = stopped.prefetch_operation(before_query);
    require(pending->io_schedule.ready_ts == origin + 45 && pending->io_schedule.batches[0].copied_page_count == 1,
            "query returning after stop still issues a full first read and one rejected copy");
    before_query.ts = origin + 46;
    stopped.complete_prefetch_io(before_query);
    stopped.drain_prefetch_queues(before_query, stopped.prefetch_queue_sizes(before_query));
    require(pending->reserved_host_pages == 0, "pre-query cancellation's physical return eventually releases its buffers");

    model::HiCacheState ranks(config);
    seed_storage(ranks, "rank:0:cache");
    seed_storage(ranks, "rank:1:cache");
    ranks.begin_formal_window(true);
    std::vector<HiCacheFact> facts;
    for (const auto * scope : { "rank:0:cache", "rank:1:cache" }) {
        for (const auto * request : { "A", "B" }) {
            auto fact = request_fact("prefetch_candidate_anchor", request, origin, 100'000);
            fact.cache_scope = scope;
            ranks.apply_fact(fact, HiCacheFactRole::PrefetchCandidateAnchor);
            facts.push_back(fact);
        }
    }
    facts[0].ts = facts[3].ts = origin + 10;
    complete_query(ranks, facts[0]);
    complete_query(ranks, facts[3]);
    const auto left = ranks.prefetch_queue_sizes(facts[0]), right = ranks.prefetch_queue_sizes(facts[3]);
    const model::HiCachePrefetchQueueSizes round{ std::min(left.revoked_operations, right.revoked_operations),
                                                  std::min(left.released_pages, right.released_pages) };
    facts[1].ts = facts[2].ts = origin + 12;
    complete_query(ranks, facts[1]);
    complete_query(ranks, facts[2]);
    facts[0].ts = facts[3].ts = origin + 20;
    ranks.drain_prefetch_queues(facts[0], round);
    ranks.drain_prefetch_queues(facts[3], round);
    require(ranks.prefetch_operation(facts[0])->reserved_host_pages == 0 && ranks.prefetch_operation(facts[3])->reserved_host_pages == 0
                && ranks.prefetch_operation(facts[1])->reserved_host_pages == 2 && ranks.prefetch_operation(facts[2])->reserved_host_pages == 2,
            "each rank drains its own FIFO prefix; newly enqueued requests are not part of the earlier MIN sample");
    require(ranks.prefetch_operation(facts[0])->prefetch_state == runtime::HiCachePrefetchState::Revoked
                && !ranks.prefetch_operation(facts[0])->payload_transfer_issued,
            "below-threshold query revokes without issuing any I/O");
    ranks.drain_prefetch_queues(facts[0], ranks.prefetch_queue_sizes(facts[0]));
    ranks.drain_prefetch_queues(facts[3], ranks.prefetch_queue_sizes(facts[3]));
    require(ranks.prefetch_operation(facts[1])->reserved_host_pages == 0 && ranks.prefetch_operation(facts[2])->reserved_host_pages == 0,
            "remaining requests are revoked and freed by the next scheduler round");

    model::HiCacheState different_hits(config);
    seed_storage(different_hits, "rank:0:cache");
    seed_storage(different_hits, "rank:1:cache", true);
    different_hits.begin_formal_window(true);
    auto a = request_fact("prefetch_candidate_anchor", "read", origin, 0), b = a;
    a.cache_scope = "rank:0:cache";
    b.cache_scope = "rank:1:cache";
    different_hits.apply_fact(a, HiCacheFactRole::PrefetchCandidateAnchor);
    different_hits.apply_fact(b, HiCacheFactRole::PrefetchCandidateAnchor);
    a.ts = origin + 10;
    b.ts = origin + 12;
    const auto hit_a = different_hits.query_prefetch_storage(a), hit_b = different_hits.query_prefetch_storage(b);
    require(hit_a == 2 && hit_b == 1 && !different_hits.prefetch_operation(a)->payload_transfer_issued,
            "local storage query must not issue payload before the cross-rank hit MIN");
    a.ts = b.ts = origin + 20;
    different_hits.complete_prefetch_query(a, std::min(hit_a, hit_b));
    different_hits.complete_prefetch_query(b, std::min(hit_a, hit_b));
    for (auto fact : { a, b }) {
        const auto * operation = different_hits.prefetch_operation(fact);
        require(operation->hit_pages.size() == 1 && operation->io_schedule.start_ts == origin + 20
                    && different_hits.prefetch_queue_sizes(fact).released_pages == 1,
                "both ranks read the common prefix and queue the remaining host reservation after MIN");
        fact.ts = origin + 25;
        (void)different_hits.stop_prefetch(fact);
        fact.ts = origin + 33;
        different_hits.publish_prefetch(fact, 0);
        fact.ts = origin + 46;
        different_hits.complete_prefetch_io(fact);
        different_hits.drain_prefetch_queues(fact, different_hits.prefetch_queue_sizes(fact));
        require(operation->reserved_host_pages == 0, "query-tail and worker-tail release must not overlap or leak pages");
    }
}

} // namespace
void check_prefetch_execution() {
    measured_stages_drive_publications();
    physical_worker_runs_on_the_dag();
    queued_workers_follow_actual_release();
    replaced_request_keeps_its_background_identity();
    prefetch_releases_follow_scheduler();
    executed_prefetch_changes_state_and_cost();
}
