#include "hicache_timing_fixture.hpp"
#include "markov/trace_graph/modules/hicache/runtime/lifecycle_observation.hpp"
#include <stdexcept>

using namespace hicache_timing_fixture;
namespace {
core::TraceEvent event(const char * name, uint64_t ts, uint64_t duration) {
    core::TraceEvent value;
    value.name = name;
    value.pid = value.tid = "worker";
    value.ts = ts; value.dur = duration;
    value.set_arg("status", "returned");
    value.set_arg("chunked", "false");
    return value;
}
}

void check_lifecycle_observations() {
    {
        runtime::HiCacheLifecycleInsert insertion{42, event("insert", 10, 100), 96, {}};
        auto boundary = [&](const char * name, uint64_t at, uint64_t first, uint64_t last, const char * id) {
            auto value = event(name, at, 3);
            value.set_arg("path_begin_tokens", std::to_string(first));
            value.set_arg("path_end_tokens", std::to_string(last));
            value.set_arg("node_id", id);
            return value;
        };
        auto hit = boundary("runtime.hicache.write_policy_check", 20, 0, 32, "1");
        auto restore = boundary("runtime.hicache.device_restore", 30, 32, 64, "2");
        auto publish = boundary("runtime.hicache.node_publish", 40, 64, 96, "3");
        insertion.node_boundaries = {publish, restore, hit};
        auto steps = runtime::lifecycle_ready_steps(insertion);
        require(steps.size() == 3 && steps[0].ready.ts == 20 && steps[1].ready.ts == 33 && steps[2].ready.ts == 43,
                "hits are ready before policy, restored and new values only after publication");
        insertion.node_boundaries.push_back(boundary("runtime.hicache.write_policy_check", 45, 64, 96, "3"));
        steps = runtime::lifecycle_ready_steps(insertion);
        require(steps.size() == 3 && steps.back().ready.ts == 45, "new leaf and later policy are one node milestone");
        const auto reject_steps = [&] {
            hicache_timing_fixture::require_throws<std::runtime_error>([&] { (void)runtime::lifecycle_ready_steps(insertion); },
                                                                       "missing, duplicate or reversed milestones cannot drive target state");
        };
        insertion.node_boundaries.pop_back();
        insertion.node_boundaries.pop_back(); reject_steps(); // Missing prefix hit.
        insertion.node_boundaries = {hit, restore, publish, hit}; reject_steps();
        restore.ts = 50;
        insertion.node_boundaries = {hit, restore, publish}; reject_steps();
        restore.ts = 30;
        insertion.node_boundaries = {hit, restore, publish, boundary("runtime.hicache.write_policy_check", 41, 64, 96, "3")};
        reject_steps(); // Check cannot start before publication returns.
    }
    core::DagGraph graph;
    auto lifecycle = event("lifecycle", 10, 90);
    lifecycle.index = 7;
    lifecycle.set_arg("phase", "end");
    lifecycle.set_arg("fact", R"({"class":"workload_identity","role":"cache_lifecycle_commit","consumers":["hicache_state_model"]})");
    graph.set_hicache_fact_events({lifecycle});
    auto fact = request_fact("cache_lifecycle_commit", "request", 100, 0);
    fact.source_event_index = 7;
    fact.source_node_id = 42;
    std::vector<model::HiCacheReplayFact> facts{{fact, HiCacheFactRole::CacheLifecycleCommit}};
    require(runtime::observe_lifecycle_inserts(graph, facts, 0, 200).empty(), "legacy trace stays without insert evidence");
    auto insert = event("runtime.hicache.radix_insert", 20, 60);
    insert.set_arg("path_tokens", "128");
    auto published = event("runtime.hicache.node_publish", 40, 2);
    published.set_arg("node_id", "91");
    published.set_arg("path_begin_tokens", "64");
    published.set_arg("path_end_tokens", "128");
    auto check = published;
    check.name = "runtime.hicache.write_policy_check";
    check.ts = 43;
    const auto reset = [&] { graph.set_runtime_observations({check, insert, published}); };
    reset();
    const auto observations = runtime::observe_lifecycle_inserts(graph, facts, 0, 200);
    require(observations.size() == 1 && observations[0].owner == 42 && observations[0].path_tokens == 128,
            "insert belongs to the replay owner, not the source node id");
    require(observations[0].node_boundaries.size() == 2 && observations[0].node_boundaries[0].name == published.name,
            "same-node publication and check survive in execution order");
    require(runtime::observe_lifecycle_inserts(graph, facts, 30, 200).empty(), "partial-window insert is not a target insertion site");
    const auto crossing = runtime::observe_lifecycle_inserts(graph, facts, 0, 41);
    require(crossing.size() == 1 && crossing.front().node_boundaries.size() == 2,
            "insert starting in the window retains its post-window policy boundary");
    graph.set_hicache_fact_events({});
    graph.set_tail_context_events({lifecycle});
    require(runtime::observe_lifecycle_inserts(graph, facts, 0, 5).empty(),
            "unmarked post-window observations are not new insertion sites");
    insert.set_arg("formal_window_context", "causal_tail");
    reset();
    const auto tail = runtime::observe_lifecycle_inserts(graph, facts, 0, 5);
    require(tail.size() == 1 && tail.front().owner == 42 && tail.front().node_boundaries.size() == 2,
            "retained post-response lifecycle keeps its insertion and publication boundaries");
    auto same_index = fact;
    same_index.source_ts = 99;
    same_index.source_node_id = 43;
    facts.push_back({same_index, HiCacheFactRole::CacheLifecycleCommit});
    require(runtime::observe_lifecycle_inserts(graph, facts, 0, 5).front().owner == 42,
            "formal and tail local indexes cannot override the matching fact time");
    facts.pop_back();
    insert.set_arg("formal_window_context", "");
    graph.set_tail_context_events({});
    graph.set_hicache_fact_events({lifecycle});
    auto restore = published;
    restore.name = "runtime.hicache.device_restore";
    graph.set_runtime_observations({insert, restore});
    const auto restored = runtime::observe_lifecycle_inserts(graph, facts, 0, 200);
    require(restored.size() == 1 && restored.front().node_boundaries.size() == 1
            && restored.front().node_boundaries.front().name == restore.name,
            "device restoration has an explicit node boundary even without a policy check");
    reset();
    const auto reject = [&] {
        hicache_timing_fixture::require_throws<std::runtime_error>([&] { (void)runtime::observe_lifecycle_inserts(graph, facts, 0, 200); },
                                                                   "invalid ownership or path metadata must not produce an insertion site");
    };
    graph.set_hicache_fact_events({lifecycle, lifecycle}); reject();
    graph.set_hicache_fact_events({lifecycle});
    graph.set_runtime_observations({insert, insert, published}); reject();
    graph.set_runtime_observations({published}); reject();
    auto bad = published; bad.tid = "other";
    graph.set_runtime_observations({insert, bad}); reject();
    bad = published; bad.set_arg("path_end_tokens", "129");
    graph.set_runtime_observations({insert, bad}); reject();
    bad = published; bad.set_arg("status", "raised");
    graph.set_runtime_observations({insert, bad}); reject();
    bad = published; bad.set_arg("chunked", "true");
    graph.set_runtime_observations({insert, bad}); reject();
    // Microsecond rounding must not admit a boundary 100 ns before its insert.
    bad = published; bad.ts = 20; bad.ts_submicro_ns = 100;
    insert.ts_submicro_ns = 200;
    graph.set_runtime_observations({insert, bad}); reject();
}
