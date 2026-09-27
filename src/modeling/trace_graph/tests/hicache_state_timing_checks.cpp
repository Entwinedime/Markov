#include "markov/trace_graph/core/cpu_gap_observation.hpp"
#include "markov/trace_graph/core/dag_builder.hpp"
#include "markov/trace_graph/modules/hicache/model/state.hpp"
#include "hicache_timing_fixture.hpp"
#include "markov/trace_graph/modules/hicache/model/replay.hpp"
#include "markov/trace_graph/modules/hicache/execution_boundaries.hpp"
#include "markov/trace_graph/modules/hicache/runtime/lifecycle_calls.hpp"
#include "markov/trace_graph/modules/hicache/patch/io_resource_model.hpp"
#include "markov/trace_graph/modules/hicache/prefetch_control.hpp"
#include "markov/trace_graph/simulation/topological_simulator.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <stdexcept>

using namespace markov::trace_graph;
using namespace markov::trace_graph::modules::hicache;
using namespace hicache_timing_fixture;
void check_prefetch_execution();
void check_prefetch_check_execution();
void check_prefetch_queue_execution();
void check_loadback_execution();

namespace {
model::HiCacheEffectDecisionLedger run_lookup(uint64_t boundary_delay, uint64_t lookup_delay, uint32_t first_token,
                                              frontend::HiCacheConfig config = timing_config(), bool queued_single_page = false) {
    model::HiCacheState state(std::move(config));
    seed_storage(state);
    state.begin_formal_window();
    state.register_prefetch_control_boundary(request_fact("cache_extend_input", "read", 200'000 + boundary_delay, first_token));
    state.apply_fact(request_fact("prefetch_candidate_anchor", "read", 200'000, first_token), HiCacheFactRole::PrefetchCandidateAnchor);
    if (queued_single_page) {
        state.register_prefetch_control_boundary(request_fact("cache_extend_input", "next", 200'000 + boundary_delay, first_token));
        auto next = request_fact("prefetch_candidate_anchor", "next", 200'030, first_token);
        next.token_count = next.full_path_span.end = next.full_path_span.token_count = 16;
        next.full_path_tokens.resize(16);
        state.apply_fact(next, HiCacheFactRole::PrefetchCandidateAnchor);
    }
    state.apply_fact(request_fact("cache_lookup_input", "read", 200'000 + lookup_delay, first_token), HiCacheFactRole::CacheLookupInput);
    state.finalize();
    return state.effect_decision_ledger();
}

void check_completion(uint64_t boundary_delay, uint64_t lookup_delay, uint32_t first_token, uint64_t completed_at) {
    const auto ledger = run_lookup(boundary_delay, lookup_delay, first_token);
    const auto load = std::ranges::find_if(ledger.decisions, [](const auto & decision) {
        return decision.request_id_provenance == "read" && decision.effect_type == model::HiCacheEffectType::Loadback;
    });
    if (load == ledger.decisions.end() || load->effective_page_count != 2) throw std::runtime_error("timing fixture must actually reload two host pages");
    if (load->eligibility_boundary.timestamp_us != 200'000 + lookup_delay) throw std::runtime_error("loadback must retain its workload opportunity timestamp");
    if (load->consumer_boundary.timestamp_us != 200'000 + completed_at)
        throw std::runtime_error("loadback service must wait for host visibility without adding source wait twice");
}

void cache_state_and_compute_share_one_execution() {
    // The canonical state (not a second cache simulator) supplies the amount
    // of work. Two us/token is an explicit synthetic test cost, not calibration.
    for (const uint64_t prior_work : { 5, 40, 5 }) {
        model::HiCacheState state(timing_config());
        core::DagGraph graph;
        const auto before = graph.add_synthetic_node({ .name = "prior request", .duration = prior_work });
        const auto first = graph.add_synthetic_node({ .name = "first extend" });
        const auto commit = graph.add_synthetic_node({ .name = "first commit" });
        const auto second = graph.add_synthetic_node({ .name = "second extend", .counts_toward_e2e = true });
        graph.add_edge(before, first, core::DagEdgeKind::Mutation);
        graph.add_edge(first, commit, core::DagEdgeKind::Mutation);
        graph.add_edge(commit, second, core::DagEdgeKind::Mutation);
        uint64_t second_at = 0;
        const uint64_t origin = 1'000'000'000;
        const auto result = simulation::run_topological_simulation(graph, [&](size_t id, uint64_t at, uint64_t duration) {
            if (id == commit) {
                auto fact = request_fact("cache_lifecycle_commit", "first", 2, 0);
                fact.ts = origin + at;
                fact.lifecycle_kind = "finished";
                state.apply_fact(fact, HiCacheFactRole::CacheLifecycleCommit);
            }
            if (id != first && id != second) return duration;
            auto fact = request_fact("cache_extend_input", id == first ? "first" : "second", id == first ? 1 : 3, 0);
            fact.ts = origin + at;
            fact.is_start = true;
            fact.is_end = false;
            if (id == second) {
                second_at = at;
                fact.token_count = fact.full_path_span.end = fact.full_path_span.token_count = 48;
                for (uint32_t token = 32; token < 48; ++token) fact.full_path_tokens.push_back({ { token } });
            }
            fact.batch_paths = {
                { .request_id = fact.request_id,
                 .full_path_span = fact.full_path_span,
                 .full_path_tokens = fact.full_path_tokens,
                 .token_count = fact.token_count }
            };
            state.apply_fact(fact, HiCacheFactRole::CacheExtendInput);
            const auto & work = state.prefill_work_items();
            if (work.empty() || work.back().request_id != fact.request_id) throw std::runtime_error("execution fixture did not reach canonical Prefill state");
            return 2 * work.back().prefill_token_count;
        });
        const auto & work = state.prefill_work_items();
        if (work.size() != 2 || work[0].prefill_token_count != 32 || work[1].prefill_token_count != 16 || work[1].reusable_prefix_token_count != 32
            || second_at != prior_work + 64 || result.e2e_us != prior_work + 96)
            throw std::runtime_error("cache state must drive compute, whose completion drives the next cache fact in the same execution");
        const auto ledger = state.effect_decision_ledger();
        const auto write = std::ranges::find_if(ledger.decisions, [](const auto & decision) {
            return decision.request_id_provenance == "first" && decision.source_fact_role == "cache_lifecycle_commit"
                   && decision.effect_type == model::HiCacheEffectType::CommitDeviceToHost;
        });
        if (write == ledger.decisions.end() || write->eligibility_boundary.timestamp_us != origin + prior_work + 64)
            throw std::runtime_error("canonical writeback eligibility must follow modeled compute completion: expected "
                                     + std::to_string(origin + prior_work + 64) + ", got "
                                     + (write == ledger.decisions.end() ? "missing" : std::to_string(write->eligibility_boundary.timestamp_us)));
    }
}

void formal_facts_keep_the_common_clock() {
    // A formal CPU gap must not put a later cache commit before prelude I/O.
    // Exercise the production parser/replay, not only HiCacheState::apply_fact.
    const auto commit = [](size_t id, uint64_t ts, const std::string & scope) {
        core::TraceEvent event;
        event.index = id;
        event.name = "hicache_commit";
        event.source_channel = core::TraceSourceChannel::PythonProbe;
        event.pid = event.tid = "worker";
        event.ts = ts;
        event.set_arg("fact", R"({"class":"workload_identity","role":"cache_lifecycle_commit","consumers":["hicache_state_model"]})");
        event.set_arg("phase", "end");
        event.set_arg("target_id", "hiradix.cache_finished_req");
        event.set_arg("request_id", std::to_string(id));
        event.set_arg("seq_no", std::to_string(id));
        event.set_arg("cache_scope", scope);
        event.set_arg("lifecycle_kind", "finished");
        event.set_arg("token_count", "32");
        event.set_arg("full_path_span",
                      nlohmann::json{
                          {     "path_id", std::to_string(id) },
                          {       "begin",                  0 },
                          {         "end",                 32 },
                          { "token_count",                 32 }
        }
                          .dump());
        std::vector<uint32_t> tokens;
        for (uint32_t i = 0; i < 32; ++i) tokens.push_back(static_cast<uint32_t>(id) * 32 + i);
        event.set_arg("token_dictionary",
                      nlohmann::json{
                          {   "path_id", std::to_string(id) },
                          { "token_ids",             tokens }
        }
                          .dump());
        return event;
    };
    for (const uint64_t delay : {0, 40}) {
        core::TraceEvent before;
        before.name = "before";
        before.pid = before.tid = "worker";
        before.ts = 100;
        before.dur = 10;
        auto after = before;
        after.ts = 300;
        auto graph = core::DagBuilder(1).build({before, after}, 0);
        auto lifecycle = commit(10, 115, "cache");
        lifecycle.dur = 85;
        graph.set_hicache_fact_events({lifecycle});
        auto insert = before;
        insert.name = "runtime.hicache.radix_insert";
        insert.ts = 120; insert.dur = 70;
        insert.set_arg("status", "returned");
        insert.set_arg("chunked", "false");
        insert.set_arg("path_tokens", "32");
        auto ready = insert;
        ready.name = "runtime.hicache.write_policy_check";
        ready.ts = 150; ready.dur = 1;
        ready.set_arg("node_id", "1");
        ready.set_arg("path_begin_tokens", "0");
        ready.set_arg("path_end_tokens", "16");
        auto second = ready;
        second.ts = 180;
        second.set_arg("node_id", "2");
        second.set_arg("path_begin_tokens", "16");
        second.set_arg("path_end_tokens", "32");
        graph.set_runtime_observations({insert, ready, second});
        model::HiCacheModelReplay replay(graph, timing_config(), true);
        runtime::HiCacheLifecycleCalls calls;
        const auto before_binding = simulation::run_topological_simulation(graph).e2e_us;
        calls.bind(graph, replay, 100, 310);
        if (!calls.owns(10)) throw std::runtime_error("complete lifecycle observations replace the end-only callback");
        if (simulation::run_topological_simulation(graph).e2e_us != before_binding)
            throw std::runtime_error("lifecycle anchors must preserve source graph timing");
        graph.set_node_duration(0, 10 + delay);
        size_t completed = 0;
        bool saw_write = false;
        (void)simulation::run_topological_simulation(graph, [&](size_t node, uint64_t elapsed, uint64_t duration) {
            for (const auto owner : calls.advance(node, 100 + elapsed, replay)) {
                if (owner != 10) throw std::runtime_error("lifecycle action changed its owner");
                for (const auto & write : replay.state().pending_device_writes(replay.facts().front().fact)) {
                    saw_write = true;
                    if (write.header.enqueue_ts != 180 + delay)
                        throw std::runtime_error("target leaf write must wait for its complete ready prefix");
                }
                if (replay.facts().front().consumed) {
                    ++completed;
                    if (100 + elapsed != 200 + delay) throw std::runtime_error("request releases only at the shifted outer return");
                }
            }
            return duration;
        });
        const auto result = replay.finish();
        if (completed != 1 || !saw_write) throw std::runtime_error("lifecycle writes and completes exactly once in DAG execution");
        for (const auto & decision : result.effect_decisions.decisions)
            if (decision.effect_type == model::HiCacheEffectType::CommitDeviceToHost
                && decision.eligibility_boundary.timestamp_us != 120 + delay)
                throw std::runtime_error("opportunity eligibility is insert entry, not the later physical write submission");
    }
    {
        core::TraceEvent before;
        before.name = "work";
        before.pid = before.tid = "worker";
        before.ts = 100;
        before.dur = 10;
        auto after = before;
        after.ts = 300;
        auto graph = core::DagBuilder(1).build({before, after}, 0);
        graph.set_hicache_fact_events({commit(10, 150, "cache")});
        graph.set_tail_context_events({commit(7, 220, "cache")});
        std::vector<size_t> seen;
        (void)model::apply_hicache_model(graph, timing_config(), [&](const HiCacheFact & fact) {
            seen.push_back(fact.source_node_id);
            return fact.source_ts;
        });
        if (seen != std::vector<size_t>{10, 18}) throw std::runtime_error("formal state must use original tail identity, not executable node count");
        const auto bindings = bind_hicache_execution_boundaries(graph);
        graph.set_node_duration(0, 60);
        (void)simulation::run_topological_simulation(graph);
        seen.clear();
        (void)model::apply_hicache_model(graph, timing_config(), [&](const HiCacheFact & fact) {
            seen.push_back(fact.source_node_id);
            const auto at = 100 + graph.node(bindings.fact_nodes.at(fact.source_node_id)).completion_time;
            if (at != fact.source_ts + 50) throw std::runtime_error("tail clock must move with its bound execution point");
            return at;
        });
        if (seen != std::vector<size_t>{10, 18}) throw std::runtime_error("inserting points must not rename facts consumed by the state clock");
    }
    for (const auto origin : { uint64_t{ 0 }, uint64_t{ 1'000'000'000 } }) {
        for (const auto & scope : { "rank:0:cache", "rank:1:cache", "cache" }) {
            core::TraceEvent before;
            before.name = "before";
            before.pid = before.tid = "worker";
            before.ts = origin + 100;
            before.dur = 10;
            auto after = before;
            after.name = "after";
            after.ts = origin + 200;
            const int rank = std::string_view(scope).starts_with("rank:1") ? 1 : 0;
            auto graph = core::DagBuilder(1).build({ before, after }, rank);
            graph.set_prelude_context_events({ commit(1, origin + 90, scope) });
            graph.set_hicache_fact_events({ commit(2, origin + 150, scope), commit(3, origin + 220, scope) });
            const auto gap = std::ranges::find_if(graph.nodes(), [](const auto & node) { return node.original_cpu_gap_after == 90; });
            if (gap == graph.nodes().end()) throw std::runtime_error("common-clock fixture must contain a 90 us CPU gap");
            for (const uint64_t owned : { 0, 45, 90 }) {
                graph.clear_scope_gap_ownership();
                graph.add_scope_gap_duration(gap->id, owned);
                const auto result = model::apply_hicache_model(graph, timing_config());
                if (!result.effect_decisions.missing_facts.empty())
                    throw std::runtime_error("common-clock fixture missing facts: " + nlohmann::json(result.effect_decisions.missing_facts).dump());
                for (const auto & [request, expected] : std::vector<std::pair<std::string, uint64_t>>{
                         { "2", origin + 150 },
                         { "3", origin + 220 }
                }) {
                    const auto found = std::ranges::find_if(result.effect_decisions.decisions, [&](const auto & decision) {
                        return decision.request_id_provenance == request && decision.effect_type == model::HiCacheEffectType::CommitDeviceToHost;
                    });
                    if (found == result.effect_decisions.decisions.end()) throw std::runtime_error("common-clock fixture must issue a writeback");
                    if (found->eligibility_boundary.timestamp_us != expected)
                        throw std::runtime_error("formal cache facts must retain their common clock, including residual CPU gaps");
                }
            }
        }
    }

    // The production state entry must consume execution time, not only expose
    // a correct timestamp in an isolated boundary helper. Two CPU lanes also
    // exercise reordering when preceding work changes on just one lane.
    for (const auto origin : { uint64_t{ 0 }, uint64_t{ 1'000'000'000 } }) {
        for (const bool separate_scopes : { false, true }) {
            std::vector<core::TraceEvent> events;
            for (const auto * tid : { "worker", "other" }) {
                core::TraceEvent before;
                before.name = "preceding_work";
                before.pid = "worker";
                before.tid = tid;
                before.ts = origin + 100;
                before.dur = 10;
                auto after = before;
                after.name = "continuation";
                after.ts = origin + 300;
                events.insert(events.end(), { before, after });
            }
            auto graph = core::DagBuilder(1).build(std::move(events), 0);
            const auto first = commit(2, origin + 150, "cache");
            auto second = commit(3, origin + 220, separate_scopes ? "other-cache" : "cache");
            second.tid = "other";
            graph.set_prelude_context_events({ commit(1, origin + 90, "cache") });
            graph.set_hicache_fact_events({ first, second });
            const auto boundaries = bind_hicache_execution_boundaries(graph);
            if (!boundaries.issues.empty() || boundaries.fact_nodes.size() != 2)
                throw std::runtime_error("production binder must connect both valid formal state facts, not prelude");
            const auto & points = boundaries.fact_nodes;
            const auto work = std::ranges::find_if(graph.nodes(), [&](const auto & node) {
                                  const auto & event = graph.event_for_node(node.id);
                                  return event.name == "preceding_work" && event.tid == "worker";
                              })->id;
            std::map<std::pair<size_t, model::HiCacheEffectType>, std::pair<std::string, std::string>> identities;
            for (const auto & decision : model::apply_hicache_model(graph, timing_config()).effect_decisions.decisions)
                identities[{ decision.source_node_id, decision.effect_type }] = { decision.effect_key, decision.cache_scope };
            for (const uint64_t delay : { 0, 60, 100, 0 }) {
                graph.set_node_duration(work, 10 + delay);
                (void)simulation::run_topological_simulation(graph);
                size_t clock_reads = 0;
                const auto result = model::apply_hicache_model(graph, timing_config(), [&](const HiCacheFact & fact) {
                    ++clock_reads;
                    if (fact.source_ts != (fact.source_node_id == 2 ? origin + 150 : origin + 220))
                        throw std::runtime_error("execution clock must not rewrite source identity or timestamp");
                    return origin + 100 + graph.node(points.at(fact.source_node_id)).completion_time;
                });
                if (clock_reads != 2 || !result.effect_decisions.missing_facts.empty())
                    throw std::runtime_error("only valid formal state facts consume execution times");
                std::map<std::string, uint64_t> epochs;
                for (const auto & decision : result.effect_decisions.decisions) {
                    if (identities.at({ decision.source_node_id, decision.effect_type }) != std::pair{ decision.effect_key, decision.cache_scope })
                        throw std::runtime_error("execution order must not rename source effects or exchange their cache scopes");
                    const auto expected = decision.request_id_provenance == "2" ? origin + 150 + delay : origin + 220;
                    if (decision.effect_type == model::HiCacheEffectType::CommitHostToStorage) {
                        // H2S exposes physical completion; the D2H consumer is
                        // instead the later write-through acknowledgement drain.
                        const auto queued = !separate_scopes && delay == 60 && decision.request_id_provenance == "3" ? 10 : 0;
                        if (decision.effective_page_count != 2 || decision.consumer_boundary.timestamp_us != expected + 40 + queued)
                            throw std::runtime_error("target fact timing must change physical I/O queue completion");
                    }
                    if (decision.effect_type != model::HiCacheEffectType::CommitDeviceToHost) continue;
                    if (decision.eligibility_boundary.timestamp_us != expected)
                        throw std::runtime_error("DAG-derived execution time must reach the formal state model");
                    epochs[decision.request_id_provenance] = decision.eligibility_boundary.epoch;
                }
                if (epochs.size() != 2 || ((epochs.at("2") < epochs.at("3")) != (delay < 70)))
                    throw std::runtime_error("target state must follow execution order, not source fact order");

                // Unlike the projected-clock check above, this state is consumed
                // during the same execution, not after a completed graph replay.
                model::HiCacheModelReplay live(graph, timing_config(), true);
                if (!live.state().effect_decision_ledger().decisions.empty())
                    throw std::runtime_error("preparing source identities must not execute future formal work");
                bool premature_finish = false;
                try { (void)live.finish(); }
                catch (const std::logic_error &) { premature_finish = true; }
                if (!premature_finish) throw std::runtime_error("unvisited formal facts cannot be reported as completed");
                std::vector<size_t> executed;
                (void)simulation::run_topological_simulation(graph, [&](size_t id, uint64_t time, uint64_t duration) {
                    for (const auto & [fact_id, point] : points) {
                        if (point != id) continue;
                        live.apply(fact_id, origin + 100 + time, id);
                        executed.push_back(fact_id);
                        const auto blockers = live.current_phase_work().blockers;
                        if (live.current_phase_work().blockers != blockers)
                            throw std::runtime_error("Reading phase work again must not accumulate input blockers");
                        const auto current = live.state().effect_decision_ledger();
                        for (const auto & decision : current.decisions)
                            if (std::ranges::find(executed, decision.source_node_id) == executed.end())
                                throw std::runtime_error("unexecuted facts must not appear in the current state");
                    }
                    return duration;
                });
                const auto live_result = live.finish();
                model::HiCacheModelReplay stepped(graph, timing_config(), true);
                for (const auto fact_id : executed) {
                    const auto & original = live.fact(fact_id);
                    const auto & item = stepped.fact(fact_id);
                    const auto time = original.fact.ts;
                    const auto point = points.at(fact_id);
                    stepped.apply(fact_id, time, point, model::HiCacheLifecycleExecution::Stepped);
                    if (!item.started || item.consumed) throw std::runtime_error("lifecycle entry is not a completed fact");
                    bool early_return = false;
                    try { stepped.return_lifecycle(fact_id, time, point); }
                    catch (const std::logic_error &) { early_return = true; }
                    if (!early_return) throw std::runtime_error("replay must not release an uninserted path");
                    stepped.advance_lifecycle(fact_id, time, point, item.fact.full_path_tokens.size());
                    if (item.consumed || item.fact.source_ts != original.fact.source_ts)
                        throw std::runtime_error("insertion preserves pending return and source identity");
                    bool unfinished = false;
                    try { (void)stepped.finish(); }
                    catch (const std::logic_error &) { unfinished = true; }
                    if (!unfinished) throw std::runtime_error("inserted facts still require their outer return");
                    bool backwards = false;
                    try { stepped.return_lifecycle(fact_id, time - 1, point); }
                    catch (const std::logic_error &) { backwards = true; }
                    if (!backwards) throw std::runtime_error("lifecycle return must obey the replay clock");
                    stepped.return_lifecycle(fact_id, time, point);
                    if (!item.consumed) throw std::runtime_error("outer return completes the fact");
                    bool repeated = false;
                    try { stepped.return_lifecycle(fact_id, time, point); }
                    catch (const std::logic_error &) { repeated = true; }
                    if (!repeated) throw std::runtime_error("outer return executes only once");
                }
                const auto stepped_result = stepped.finish();
                if (stepped_result.effect_decisions.decisions.size() != live_result.effect_decisions.decisions.size())
                    throw std::runtime_error("staged lifecycle does not duplicate effects");
                if (executed != (delay < 70 ? std::vector<size_t>{2, 3} : std::vector<size_t>{3, 2}))
                    throw std::runtime_error("state callbacks must follow changed cross-thread execution order");
                for (const auto & decision : live_result.effect_decisions.decisions) {
                    if (identities.at({decision.source_node_id, decision.effect_type}) != std::pair{decision.effect_key, decision.cache_scope})
                        throw std::runtime_error("live dispatch must retain formal source effect identities");
                    if (decision.effect_type == model::HiCacheEffectType::CommitDeviceToHost) {
                        const auto expected = decision.request_id_provenance == "2" ? origin + 150 + delay : origin + 220;
                        if (decision.eligibility_boundary.timestamp_us != expected)
                            throw std::runtime_error("live state must consume the current execution boundary");
                    }
                }
                bool duplicate = false;
                try { live.apply(2, origin + 400); }
                catch (const std::logic_error &) { duplicate = true; }
                if (!duplicate) throw std::runtime_error("completed replay must not accept another fact");
            }
            bool missing_boundary = false;
            try {
                (void)model::apply_hicache_model(graph, timing_config(), [](const HiCacheFact &) -> uint64_t {
                    throw std::out_of_range("execution boundary unavailable");
                });
            }
            catch (const std::out_of_range &) {
                missing_boundary = true;
            }
            if (!missing_boundary) throw std::runtime_error("a missing target boundary cannot silently fall back to source time");
        }
    }
}
} // namespace

void check_hicache_state_timing() {
    check_prefetch_execution();
    check_prefetch_check_execution();
    check_prefetch_queue_execution();
    check_loadback_execution();
    cache_state_and_compute_share_one_execution();
    formal_facts_keep_the_common_clock();
    auto config = timing_config();
    config.io_cost.storage_batch_pages = 2;
    config.prefetch_policy = "timeout";
    config.prefetch_timeout_configured = true;
    // Two pages: allocate/read 20us, publish at 25/30us, cleanup until 40us.
    // The state must see pages published strictly before cancellation, without
    // waiting for batch cleanup. The formal check time is still a policy proxy.
    config.io_cost.service_models.at("prefetch").stages = { 10, 0, 5, 0, 10, 0 };
    for (const auto [deadline, pages] : {
             std::pair{ .000027, 1ULL },
             std::pair{ .000030, 1ULL },
             std::pair{ .000031, 2ULL }
    }) {
        config.prefetch_timeout_base_sec = config.prefetch_timeout_max_sec = deadline;
        const auto ledger = run_lookup(100, 100, 0, config);
        const auto prefetch = std::ranges::find_if(ledger.decisions, [](const auto & decision) {
            return decision.request_id_provenance == "read" && decision.effect_type == model::HiCacheEffectType::PrefetchIo;
        });
        if (prefetch == ledger.decisions.end() || prefetch->completed_page_count != pages)
            throw std::runtime_error("formal cache state must use published pages before physical batch return");
    }
    config.prefetch_timeout_base_sec = config.prefetch_timeout_max_sec = .000022;
    const auto cancelled = run_lookup(100, 100, 0, config);
    const auto costs = patch::build_hicache_io_resource_plan(cancelled, config.io_cost);
    const auto cost = std::ranges::find_if(costs.costs, [](const auto & item) { return item.effect_type == model::HiCacheEffectType::PrefetchIo; });
    // Read both pages, copy the first, reject its increment, then release the
    // temporary batch. The second copy must not remain in the DAG service.
    if (cost == costs.costs.end() || cost->duration_us != 35 || cost->effective_page_count != 2)
        throw std::runtime_error("cancelled state execution must reach DAG cost as two reads, one copy, and cleanup");
    const auto decision = std::ranges::find_if(cancelled.decisions, [](const auto & item) { return item.effect_type == model::HiCacheEffectType::PrefetchIo; });
    if (cost->storage_service_batches.size() != 1 || cost->storage_service_batches.front().copied_page_count != 1 || decision->completed_page_count != 0)
        throw std::runtime_error("published prefix must not be confused with physical reads or the rejected final copy");
    const auto queued = run_lookup(100, 100, 0, config, true);
    const auto next = std::ranges::find_if(queued.decisions, [](const auto & item) {
        return item.request_id_provenance == "next" && item.effect_type == model::HiCacheEffectType::PrefetchIo;
    });
    // First releases at 35 rather than 40; queued one-page work publishes at
    // 35+10+5=50, before its deadline 30+22=52. Keeping the old full-batch
    // release would wrongly cancel it at 52 before its publication at 55.
    if (next == queued.decisions.end() || next->effective_page_count != 1 || next->completed_page_count != 1)
        throw std::runtime_error("next prefetch must start after actual cancelled resource release, not full-batch completion");
    // The first path is now storage-only. Publishing its prefetch takes until
    // max(control boundary, I/O completion); H2D cannot execute before that.
    check_completion(100, 5, 0, 120);
    check_completion(5, 5, 0, 40);
    check_completion(100, 150, 0, 170);
    // A retained host path needs no prefetch. A later control boundary cannot
    // delay its loadback merely because this request had a candidate anchor.
    check_completion(100, 5, 8 * 32, 25);
}
