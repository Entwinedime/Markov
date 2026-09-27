#include "hicache_timing_fixture.hpp"
#include "markov/trace_graph/core/dag_builder.hpp"
#include "markov/trace_graph/modules/hicache/execution_boundaries.hpp"
#include "markov/trace_graph/modules/hicache/model/replay.hpp"
#include "markov/trace_graph/simulation/topological_simulator.hpp"
#include <nlohmann/json.hpp>
#include <stdexcept>

using namespace hicache_timing_fixture;
namespace {
HiCacheFact extend() {
    auto fact = request_fact("cache_extend_input", "active", 15, 100);
    fact.is_start = true;
    fact.is_end = false;
    fact.batch_kind = "extend";
    fact.batch_size = 1;
    fact.batch_paths = {
        { .request_id = "active", .full_path_span = fact.full_path_span, .full_path_tokens = fact.full_path_tokens, .token_count = 32 }
    };
    return fact;
}
core::TraceEvent event(const char * name, uint64_t ts, uint64_t duration) {
    core::TraceEvent e;
    e.name = name;
    e.pid = e.tid = "worker";
    e.ts = ts;
    e.dur = duration;
    return e;
}
core::TraceEvent allocation(uint64_t ts, uint64_t iteration) {
    auto e = event("runtime.hicache.decode_allocation", ts, 2);
    e.index = iteration;
    e.set_arg("status", "returned");
    e.set_arg("token_per_req", "1");
    e.set_arg("cache_scope", "cache");
    e.set_arg("requests",
              nlohmann::json::array({
                                        { { "request_id", "active" },
                                         { "decode_batch_idx", iteration },
                                         { "kv_committed_len", 32 + iteration },
                                         { "kv_allocated_len", 32 + iteration } }
    })
                  .dump());
    return e;
}
core::TraceEvent forward(uint64_t ts) {
    auto e = event("runtime.hicache.layer_waits", ts, 5);
    e.set_arg("phase", "DECODE");
    e.set_arg("status", "returned");
    e.set_arg("request_ids", R"(["active"])");
    return e;
}
std::vector<model::HiCacheReplayFact> actions(core::DagGraph & graph) {
    const std::vector<model::HiCacheReplayFact> facts{
        { extend(), HiCacheFactRole::CacheExtendInput }
    };
    HiCachePhaseObservationAudit phases;
    phases.observations.push_back({ .pid = "worker", .request_ids = { "active" }, .batch_size = 1, .decode_iteration_count = 2 });
    return model::observe_decode_allocations(graph, facts, phases, 10, 101);
}
void observations_and_boundaries() {
    auto graph = core::DagBuilder(1).build({ event("before", 10, 1), event("after", 100, 1) }, 0);
    const std::vector<core::TraceEvent> observed{ allocation(30, 0), forward(35), allocation(60, 1), forward(65) };
    graph.set_runtime_observations(observed);
    const auto derived = actions(graph);
    require(derived.size() == 2 && derived[0].fact.source_node_id != extend().source_node_id
                && derived[0].fact.source_node_id != derived[1].fact.source_node_id,
            "decode has independent semantic identities");
    require(derived[0].fact.batch_paths[0].full_path_tokens == extend().batch_paths[0].full_path_tokens,
            "allocation keeps approved workload provenance, not source page outcomes");
    require(parse_hicache_fact_role("cache_decode_allocation") == HiCacheFactRole::Unknown, "derived execution action does not widen raw probe fact routing");
    const auto before = simulation::run_topological_simulation(graph).e2e_us;
    const auto original = graph.nodes();
    const auto bound = bind_hicache_execution_boundaries(graph, derived);
    require(bound.issues.empty() && bound.fact_nodes.size() == 2, "allocation points bind at observed CPU entries");
    require(simulation::run_topological_simulation(graph).e2e_us == before, "allocation envelopes are not additional costs");
    for (const auto & node : original) require(graph.node(node.id).completion_time == node.completion_time, "binding leaves original work unchanged");
    for (int fault = 0; fault < 6; ++fault) {
        auto changed = observed;
        if (fault == 0) changed.erase(changed.begin());
        if (fault == 1) changed[2] = allocation(60, 0);
        if (fault == 2) changed[0].set_arg("cache_scope", "other-cache");
        if (fault == 3) changed[0].set_arg("token_per_req", "2");
        if (fault == 4) changed[0].set_arg("status", "raised");
        if (fault == 5) changed[1].ts = 31; // Forward starts before allocation returns.
        graph.set_runtime_observations(std::move(changed));
        hicache_timing_fixture::require_throws<std::exception>([&] { (void)actions(graph); },
                                                               "incomplete or inconsistent decode evidence must not silently fall back");
    }
}
uint64_t written_pages(const model::HiCacheState & state, size_t fact_id) {
    uint64_t pages = 0;
    for (const auto & decision : state.effect_decision_ledger().decisions)
        if (decision.source_node_id == fact_id && decision.effect_type == model::HiCacheEffectType::CommitDeviceToHost) pages += decision.effective_page_count;
    return pages;
}
void per_iteration_capacity(bool overlap_commit) {
    auto config = timing_config();
    config.write_policy = "write_back";
    model::HiCacheState state(config);
    auto seed = request_fact("cache_lifecycle_commit", "seed", 1, 0);
    seed.token_count = seed.full_path_span.end = seed.full_path_span.token_count = 16;
    seed.full_path_tokens.resize(16);
    seed.lifecycle_kind = "finished";
    state.apply_fact(seed, HiCacheFactRole::CacheLifecycleCommit, false);
    state.finalize();
    state.begin_formal_window(true);
    const auto input = extend();
    state.apply_fact(input, HiCacheFactRole::CacheExtendInput);
    auto graph = core::DagBuilder(1).build({ event("before", 10, 1), event("after", 100, 1) }, 0);
    graph.set_runtime_observations({ allocation(30, 0), forward(35), allocation(60, 1), forward(65) });
    const auto decoded = actions(graph);
    state.apply_fact(decoded[0].fact, decoded[0].role);
    require(written_pages(state, decoded[0].fact.source_node_id) == 0, "first decode consumes the last free page without eviction");
    if (overlap_commit) {
        // The overlap loop starts decode before processing the previous
        // prefill result. That result only publishes the prompt's full pages;
        // the already allocated decode tail remains owned by this request.
        auto unfinished = request_fact("cache_lifecycle_commit", "active", 40, 100);
        unfinished.lifecycle_kind = "unfinished";
        state.apply_fact(unfinished, HiCacheFactRole::CacheLifecycleCommit);
    }
    state.apply_fact(decoded[1].fact, decoded[1].role);
    require(written_pages(state, decoded[1].fact.source_node_id) == 1, "second decode adds zero pages but still evicts under the per-request budget");
    hicache_timing_fixture::require_throws<std::runtime_error>([&] { state.apply_fact(decoded[1].fact, decoded[1].role, false); },
                                                               "same decode iteration cannot allocate twice");
    require(state.allocation_pending(decoded[1].fact), "zero-new-page decode still waits for its eviction writeback");
    auto resume = decoded[1].fact;
    resume.ts = 80;
    complete_test_writes(state, resume);
    state.resume_allocation(resume);
    auto commit = request_fact("cache_lifecycle_commit", "active", 90, 100);
    commit.token_count = commit.full_path_span.end = commit.full_path_span.token_count = 34;
    commit.full_path_tokens.push_back({ { 132 } });
    commit.full_path_tokens.push_back({ { 133 } });
    commit.lifecycle_kind = "finished";
    state.apply_fact(commit, HiCacheFactRole::CacheLifecycleCommit);
    require(written_pages(state, commit.source_node_id) == 0, "lifecycle only inserts/releases; it does not allocate decode pages again");
    model::HiCacheState missing(config);
    missing.begin_formal_window(true);
    missing.apply_fact(input, HiCacheFactRole::CacheExtendInput);
    hicache_timing_fixture::require_throws<std::runtime_error>([&] { missing.apply_fact(commit, HiCacheFactRole::CacheLifecycleCommit); },
                                                               "request-end cannot replace a missing live decode allocation");
}
void target_page_size_controls_demand() {
    for (const uint64_t page_size : { 16, 32 }) {
        auto config = timing_config();
        config.page_size = page_size;
        config.l1_capacity_pages = 16;
        model::HiCacheState state(config);
        state.begin_formal_window(true);
        auto input = extend();
        auto & path = input.batch_paths.front();
        for (uint32_t token = 132; token < 148; ++token) path.full_path_tokens.push_back({ { token } });
        path.token_count = path.full_path_span.end = path.full_path_span.token_count = 48;
        state.apply_fact(input, HiCacheFactRole::CacheExtendInput);
        auto decode = input;
        decode.role = "cache_decode_allocation";
        decode.source_node_id = decode.source_event_index = 30;
        decode.ts = decode.source_ts = 30;
        decode.source_page_size = 128; // Never used as target allocation geometry.
        decode.decode_iterations = { 0 };
        state.apply_fact(decode, HiCacheFactRole::CacheDecodeAllocation);
        auto unfinished = request_fact("cache_lifecycle_commit", "active", 40, 100);
        unfinished.full_path_tokens = path.full_path_tokens;
        unfinished.full_path_span = path.full_path_span;
        unfinished.token_count = path.token_count;
        unfinished.lifecycle_kind = "unfinished";
        state.apply_fact(unfinished, HiCacheFactRole::CacheLifecycleCommit);
        auto next = extend();
        next.source_node_id = next.source_event_index = 50;
        next.ts = next.source_ts = 50;
        next.batch_paths.front().request_id = "next";
        state.apply_fact(next, HiCacheFactRole::CacheExtendInput);
        const auto expected = page_size == 16 ? 4 : 2;
        require(state.allocator_work_items().back().free_index_offset == expected,
                "unfinished commit preserves the allocated tail and free-list position for either target page size");
    }
}
} // namespace

void check_decode_allocations() {
    observations_and_boundaries();
    per_iteration_capacity(false);
    per_iteration_capacity(true);
    target_page_size_controls_demand();
}
