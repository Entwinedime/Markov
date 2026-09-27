#include "hicache_timing_fixture.hpp"
#include "markov/trace_graph/modules/hicache/cpu_collective_timing.hpp"
#include "markov/trace_graph/modules/hicache/runtime/load_consumers.hpp"
#include <stdexcept>
using namespace hicache_timing_fixture;

namespace {
constexpr uint64_t origin = 200'000;
void consumers_follow_forward_not_dma_lifetime() {
    runtime::HiCacheLoadConsumers consumers;
    const auto rejected = [&](auto action) {
        hicache_timing_fixture::require_throws<std::runtime_error>([&] { action(); }, "a consumer must not be inferred without its causal batch boundary");
    };
    rejected([&] { (void)consumers.forward("rank0", "first", false); });
    rejected([&] { (void)consumers.forward("rank0", "first", true); });
    consumers.submitted("rank0", { 10, 20 });
    consumers.submitted("rank1", {});
    rejected([&] { consumers.submitted("rank0", { 99 }); });
    require(consumers.forward("rank0", "first", true) == std::vector<size_t>{ 10, 20 }, "EXTEND receives this rank's submitted layer events");
    require(consumers.forward("rank1", "first", true).empty(), "the same request can have an inactive consumer on another rank");
    // DMA completion and scheduler ACK do not reset a forward's consumer.
    for (size_t iteration = 0; iteration < 8; ++iteration)
        require(consumers.forward("rank0", "first", false) == std::vector<size_t>{ 10, 20 }, "Decode retains the preceding EXTEND consumer");
    consumers.submitted("rank0", {});
    require(consumers.forward("rank0", "first", false).size() == 2, "another queue drain does not revise an admitted forward");
    require(consumers.forward("rank0", "second", true).empty(), "an empty load creates an inactive new consumer");
    require(consumers.forward("rank0", "second", false).empty(), "inactive Decode stays inactive");
    rejected([&] { (void)consumers.forward("rank0", "third", true); });
    consumers.submitted("rank0", { 30 });
    require(consumers.forward("rank0", "second", true) == std::vector<size_t>{ 30 }, "a new EXTEND replaces the same request's prior batch consumer");
}
HiCacheFact queue_load(model::HiCacheState & state, const std::string & request, uint32_t token, uint64_t at, const std::string & scope = "cache") {
    auto fact = request_fact("prefetch_candidate_anchor", request, origin + at, token);
    fact.cache_scope = scope;
    state.apply_fact(fact, HiCacheFactRole::PrefetchCandidateAnchor);
    fact.role = "cache_lookup_input";
    fact.source_node_id = fact.source_event_index = fact.ts + 1;
    state.apply_fact(fact, HiCacheFactRole::CacheLookupInput);
    return fact;
}

void load_min_uses_device_callbacks() {
    auto config = timing_config();
    config.l1_capacity_pages = 8;
    config.l2_capacity_pages = 24;
    config.io_cost.service_models.at("load").page_bandwidth_points.front().setup_us_per_operation = 7;
    model::HiCacheState state(config);
    for (const int rank : { 0, 1 }) seed_storage(state, "rank:" + std::to_string(rank) + ":cache");
    state.begin_formal_window(true);
    std::map<int, HiCacheFact> facts;
    for (const int rank : { 0, 1 }) {
        const auto scope = "rank:" + std::to_string(rank) + ":cache";
        facts[rank] = queue_load(state, "first", 0, 0, scope);
        (void)queue_load(state, "second", 32, 10, scope);
    }
    // The two candidates are already queued by t=10; execute from t=20.
    core::DagGraph graph;
    const auto root = graph.add_synthetic_node({ .name = "queued loads", .counts_toward_e2e = true });
    const auto point = [&](int rank, uint64_t at) {
        const auto delay = graph.add_synthetic_node({ .name = "ordinary CPU", .duration = at - 20 });
        const auto gate = graph.add_synthetic_node({ .name = "scheduler boundary" });
        graph.mutable_node(gate).gpu_id = rank;
        graph.add_edge(root, delay, core::DagEdgeKind::Mutation);
        graph.add_edge(delay, gate, core::DagEdgeKind::Mutation);
        return gate;
    };
    std::map<size_t, std::pair<int, bool>> submissions;
    std::map<size_t, std::pair<int, uint64_t>> completions;
    struct Check {
        size_t round;
        int rank;
        bool apply;
    };
    std::map<size_t, Check> actions;
    std::vector<CpuRankNodes> checks(3);
    std::map<size_t, std::map<int, uint64_t>> samples;
    CpuCollectiveTiming communication;
    for (const int rank : { 0, 1 }) {
        submissions[point(rank, 100 + 10 * rank)] = { rank, false };
        submissions[point(rank, 165)] = { rank, true };
        for (const auto [round, at] : std::vector<std::pair<size_t, uint64_t>>{
                 { 0, 150 },
                 { 1, 185 },
                 { 2, 210 }
        }) {
            checks[round][rank] = point(rank, at);
            actions[checks[round][rank]] = { round, rank, false };
        }
        communication.calls[rank] = {
            .entry_ranks = { 0, 1 },
            .worker_remainder = 8 + 4UL * rank
        };
    }
    size_t returns = 0;
    (void)simulation::run_topological_simulation(graph, {}, [&](size_t node, uint64_t elapsed, simulation::FutureDag & future) {
        const auto time = origin + 20 + elapsed;
        if (node == root)
            for (size_t i = 0; i < checks.size(); ++i)
                for (const auto & [rank, call] : append_cpu_collective(communication, checks[i], future)) {
                    const auto after = future.append({ .name = "load MIN returned" }, simulation::BoundaryOrder::Ordinary, checks[i].at(rank));
                    future.depend(call, after);
                    actions[after] = { i, rank, true };
                }
        if (const auto at = submissions.find(node); at != submissions.end()) {
            const auto [rank, later] = at->second;
            auto fact = facts.at(rank);
            fact.ts = time;
            if (later) (void)queue_load(state, "third", 64, time - origin, fact.cache_scope);
            const auto batch = state.submit_loadbacks(fact);
            require(batch.has_value(), "explicit source of target load work must exist");
            const auto service = future.append({ .name = "synthetic calibrated H2D", .duration = batch->io_schedule.ready_ts - time });
            const auto complete = future.append({ .name = "device completion" });
            future.depend(service, complete);
            completions[complete] = { rank, batch->id };
        }
        if (const auto at = completions.find(node); at != completions.end()) {
            auto fact = facts.at(at->second.first);
            fact.ts = time;
            state.complete_loadback_batch(fact, at->second.second);
        }
        if (const auto at = actions.find(node); at != actions.end()) {
            const auto [round, rank, apply] = at->second;
            auto fact = facts.at(rank);
            fact.ts = time;
            if (!apply) samples[round][rank] = state.load_completion_count(fact);
            else {
                require(samples.at(round).size() == 2, "load MIN must include both device-completion samples");
                const auto common = std::min(samples.at(round).at(0), samples.at(round).at(1));
                state.acknowledge_loads(fact, common);
                require(state.load_completion_count(fact) == (round == 2 ? 0 : 1), "late completion must survive the older MIN sample");
                ++returns;
            }
        }
    });
    require(returns == 6 && samples.at(0).at(0) == 1 && samples.at(0).at(1) == 0,
            "asymmetric physical completion must propagate through three actual MIN rounds");
    hicache_timing_fixture::require_static_replay(graph, "load submission, device completion and MIN work must survive static replay");
}
void loads_merge_at_submission() {
    auto config = timing_config();
    config.l1_capacity_pages = 8;
    config.l2_capacity_pages = 24;
    config.io_cost.service_models.at("load").page_bandwidth_points.front().setup_us_per_operation = 7;
    model::HiCacheState state(config);
    seed_storage(state);
    state.begin_formal_window(true);
    auto fact = queue_load(state, "first", 0, 0);
    const auto other = queue_load(state, "second", 32, 10);
    const auto shared = queue_load(state, "shared-prefix", 0, 15);
    const auto * admission = state.load_admission_work(fact);
    require(admission && admission->allocated && admission->requested_pages == 2 && admission->node_pages == std::vector<uint64_t>{ 2 },
            "a direct successful load retains target allocation and clone geometry");
    require(!state.allocation_pending(fact) && !state.load_admission_work(shared), "shared dependencies and capacity waiting are not new load admission work");
    auto wrong_scope = fact;
    wrong_scope.cache_scope = "unrelated-cache";
    require(!state.load_admission_work(wrong_scope), "load admission cannot cross cache scopes");
    const auto first_dependency = state.loadback_dependencies(fact);
    require(first_dependency.size() == 1 && state.loadback_dependencies(shared) == first_dependency,
            "logical device hit by another request must retain the queued load dependency");
    require(state.loadback_dependencies(other).size() == 1 && state.loadback_dependencies(other) != first_dependency,
            "independent prefixes must not inherit another request's load");
    auto split = request_fact("cache_lookup_input", "short-prefix", origin + 16, 0);
    split.token_count = split.full_path_span.end = split.full_path_span.token_count = 16;
    split.full_path_tokens.resize(16);
    state.apply_fact(split, HiCacheFactRole::CacheLookupInput);
    require(state.load_admission_work(fact)->node_pages == std::vector<uint64_t>{ 2 }, "later radix splitting cannot revise an earlier clone count");
    require(state.loadback_dependencies(split) == first_dependency, "radix split must retain the in-flight page dependency");
    fact.ts = origin + 100;
    require(!state.load_completion_count(fact), "queued loads cannot complete before start_loading");
    for (const auto & d : state.effect_decision_ledger().decisions)
        if (d.effect_type == model::HiCacheEffectType::Loadback) {
            const auto expected = d.request_id_provenance == "first" || d.request_id_provenance == "second" ? 2 : 0;
            require(d.effective_page_count == expected && !d.completed_page_count, "shared hits must not issue duplicate I/O or fabricate completion");
        }
    auto controller = fact;
    controller.request_id.clear();
    const auto first = state.submit_loadbacks(controller);
    require(first && first->operations.size() == 2 && first->io_schedule.start_ts == fact.ts && first->io_schedule.duration_us == 47
                && first->io_schedule.effective_byte_count == 64,
            "two queued two-page loads must share one setup plus four-page transfer, only at submission");
    require(state.loadback_dependencies(shared) == first_dependency
                && std::ranges::find(first->operations, first_dependency.front()) != first->operations.end(),
            "submission preserves the operation identity used to find layer-ready gates");
    require(!state.submit_loadbacks(controller), "requestless empty start_loading must not create another batch");
    hicache_timing_fixture::require_throws<std::logic_error>([&] { state.complete_loadback_batch(fact, first->id); },
                                                             "a submitted DMA cannot complete before its service");
    (void)queue_load(state, "third", 64, 110);
    fact.ts = origin + 120;
    const auto second = state.submit_loadbacks(fact);
    require(second && second->operations.size() == 1 && second->id != first->id && second->io_schedule.start_ts == origin + 147
                && second->io_schedule.ready_ts == origin + 174,
            "later submission must form its own batch behind the first DMA");
    fact.ts = origin + 200;
    require(!state.load_completion_count(fact), "advancing wall time alone cannot fabricate a device completion callback");
    fact.ts = origin + 147;
    state.complete_loadback_batch(fact, first->id);
    require(state.loadback_dependencies(shared).empty() && state.loadback_dependencies(split).empty(),
            "physical completion allows compute before scheduler acknowledgement");
    const auto sampled = state.load_completion_count(fact);
    require(sampled == 1, "a completed two-operation batch contributes one, not two, to MIN");
    fact.ts = origin + 174;
    state.complete_loadback_batch(fact, second->id);
    fact.ts = origin + 180;
    state.acknowledge_loads(fact, sampled);
    require(state.load_completion_count(fact) == 1, "batch completed during MIN must remain for the next check");
    state.acknowledge_loads(fact, 1);
    require(!state.load_completion_count(fact), "next MIN releases the remaining batch");
    for (const auto & d : state.effect_decision_ledger().decisions)
        if (d.effect_type == model::HiCacheEffectType::Loadback)
            require(d.completed_page_count == d.effective_page_count, "device callback completes members, not duplicate loads for shared hits");
    state.begin_formal_window(true);
    require(!state.load_admission_work(fact), "a new formal window discards previous admission work");
}

void load_admission_does_not_publish_failed_allocation() {
    auto config = timing_config();
    config.l1_capacity_pages = 2;
    config.l2_capacity_pages = 24;
    model::HiCacheState state(config);
    seed_storage(state);
    state.begin_formal_window(true);
    const auto first = queue_load(state, "holds-device-pages", 0, 0);
    auto second = queue_load(state, "cannot-fit", 32, 10);
    require(state.load_admission_work(first)->allocated, "first load occupies the available device pages");
    const auto * work = state.load_admission_work(second);
    require(work && work->requested_pages == 2 && !work->allocated && !state.allocation_pending(second),
            "capacity failure without pending writes returns without claiming successful clone work");
    require(state.loadback_dependencies(second).empty(), "failed admission must not create an in-flight load");
    require(!state.load_allocation_will_succeed(second), "capacity exhaustion selects a failed retry, not successful index work");
}

void completed_load_stays_pinned_until_ack() {
    auto config = timing_config();
    config.prefetch_threshold_pages = config.prefetch_capacity_limit_pages = 8;
    model::HiCacheState state(config);
    seed_storage(state);
    state.begin_formal_window(true);
    auto load = queue_load(state, "load", 256, 0); // host-resident, no longer in L1 after seeding.
    load.ts = origin + 5;
    const auto batch = state.submit_loadbacks(load);
    require(batch && batch->operations.size() == 1, "fixture must submit one host load");
    load.ts = batch->io_schedule.ready_ts;
    state.complete_loadback_batch(load, batch->id);
    auto finished = request_fact("cache_lifecycle_commit", "load", load.ts + 1, 256);
    finished.lifecycle_kind = "finished";
    state.apply_fact(finished, HiCacheFactRole::CacheLifecycleCommit);
    const auto extend = [&](const char * request, uint32_t token, uint64_t at) {
        auto fact = request_fact("cache_extend_input", request, at, token);
        fact.is_start = true;
        fact.is_end = false;
        fact.batch_paths = {
            { .request_id = fact.request_id, .full_path_span = fact.full_path_span, .full_path_tokens = fact.full_path_tokens, .token_count = fact.token_count }
        };
        state.apply_fact(fact, HiCacheFactRole::CacheExtendInput);
    };
    extend("compute-before-ack", 20'000, load.ts + 2); // Can evict ordinary L1 data, but not the unacknowledged load.
    const auto pressure = [&](const char * request, uint64_t at) {
        auto fact = request_fact("prefetch_candidate_anchor", request, at, 10'000);
        fact.token_count = fact.full_path_span.end = fact.full_path_span.token_count = 128;
        for (uint32_t token = 10'032; token < 10'128; ++token) fact.full_path_tokens.push_back({ { token } });
        state.apply_fact(fact, HiCacheFactRole::PrefetchCandidateAnchor);
        const auto * op = state.prefetch_candidate_operation(fact);
        return op ? op->reserved_host_pages : 0;
    };
    require(!pressure("before-ack", load.ts + 3), "device completion and request completion must retain the unacknowledged load pin");
    load.ts += 4;
    state.finalize();
    require(state.load_completion_count(load) == 1, "finish cannot consume load acknowledgement");
    state.acknowledge_loads(load, 1);
    extend("compute-after-ack", 30'000, load.ts + 1); // Device eviction now makes the old host leaf eligible as well.
    require(pressure("after-ack", load.ts + 2) == 8, "scheduler ACK must restore the device/host eviction needed for allocation");
}
} // namespace

void check_loadback_execution() {
    consumers_follow_forward_not_dma_lifetime();
    load_min_uses_device_callbacks();
    loads_merge_at_submission();
    load_admission_does_not_publish_failed_allocation();
    completed_load_stays_pinned_until_ack();
}
