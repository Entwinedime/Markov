#include "hicache_timing_fixture.hpp"
#include "markov/trace_graph/core/dag_builder.hpp"
#include "markov/trace_graph/modules/hicache/model/prefetch_check_execution.hpp"
#include "markov/trace_graph/modules/hicache/model/prefetch_execution.hpp"
#include "markov/trace_graph/modules/hicache/model/prefetch_scheduler_body.hpp"
#include "markov/trace_graph/modules/hicache/model/prefetch_wait_execution.hpp"
#include "markov/trace_graph/modules/hicache/patch/io_operation_ledger.hpp"
#include "markov/trace_graph/modules/hicache/patch/prefetch_control_region.hpp"
#include "markov/trace_graph/modules/hicache/prefetch_control.hpp"
#include "markov/trace_graph/simulation/topological_simulator.hpp"
#include <algorithm>
#include <stdexcept>

using namespace markov::trace_graph;
namespace hc = modules::hicache;
namespace {
using hicache_timing_fixture::require;

core::TraceEvent event(std::string name, uint64_t ts, uint64_t dur, std::string pid = "1", std::string tid = "1") {
    core::TraceEvent e;
    e.name = std::move(name);
    e.ts = ts;
    e.dur = dur;
    e.pid = std::move(pid);
    e.tid = std::move(tid);
    e.cat = "cpu_op";
    e.source_channel = core::TraceSourceChannel::Torch;
    return e;
}
std::vector<core::TraceEvent> fixture(bool done = true, bool best = false) {
    std::vector<core::TraceEvent> events;
    for (int rank = 0; rank < 2; ++rank) {
        const auto pid = std::to_string(rank + 1);
        events.push_back(event("before", 0, 1, pid, pid));
        const auto append = [&](const char * name, uint64_t at, uint64_t dur) -> core::TraceEvent & {
            auto e = event(name, at, dur, pid, pid);
            e.source_channel = core::TraceSourceChannel::PythonProbe;
            if (e.name.starts_with("runtime.")) e.cat = "runtime_diagnostic";
            e.set_arg("request_id", "request");
            e.set_arg("status", "returned");
            events.push_back(std::move(e));
            return events.back();
        };
        append("hicache.control.prefetch_progress", 100, 100);
        auto & check = append("runtime.hicache.prefetch_check", 105, best ? 5 : 35);
        check.set_arg("can_terminate", done ? "true" : "false");
        if (done) append("runtime.hicache.prefetch_stop", 145, 5).set_arg("completed_tokens", "128");
        for (int part = 0; part < 2; ++part) {
            if ((part == 0 && best) || (part == 1 && !done)) continue;
            const uint64_t start = part == 0 ? 110 : 160;
            events.push_back(event("c10d::allreduce_", start + 2, 3, pid, pid));
            events.push_back(event("gloo:all_reduce", start + 4, 10, pid, "worker"));
            auto & call = append("runtime.cpu_collective", start, 20);
            hicache_timing_fixture::set_collective_args(call, rank, part, "group");
            call.set_arg("numel", part == 0 ? "2" : "1");
            call.set_arg("reduce_op", part == 0 ? "RedOpType.MAX" : "RedOpType.MIN");
            call.set_arg("role", part == 0 ? "prefetch_state_check" : "prefetch_completion");
        }
        auto & fact = append("progress fact", 100, 100);
        fact.set_arg("fact", R"({"class":"source_actual","role":"prefetch_progress_observed","consumers":["hicache_dag_patch"]})");
        fact.set_arg("phase", "end");
        fact.set_arg("progress_ready", done ? "true" : "false");
    }
    return events;
}
std::vector<core::TraceEvent> with_local_returns() {
    auto events = fixture();
    const auto active = events;
    for (auto e : active) {
        if (e.name != "hicache.control.prefetch_progress" && e.name != "progress fact") continue;
        e.ts = 300;
        e.dur = 7;
        e.set_arg("request_id", "no-operation");
        events.push_back(std::move(e));
    }
    return events;
}
core::DagGraph build(std::vector<core::TraceEvent> events) {
    for (size_t i = 0; i < events.size(); ++i) events[i].index = i;
    std::vector<core::TraceEvent> facts;
    std::erase_if(events, [&](const auto & event) {
        if (event.arg("fact").empty()) return false;
        facts.push_back(event);
        facts.back().index = facts.size() - 1;
        return true;
    });
    auto graph = core::DagBuilder(1).build(std::move(events), 0);
    graph.set_hicache_fact_events(std::move(facts));
    return graph;
}
auto observe(const core::DagGraph & graph, std::string_view policy = "timeout") {
    const hc::patch::HiCacheSourceDagIndex source(graph);
    return hc::observe_prefetch_control(source, hc::observe_cpu_collectives(source), policy);
}

std::vector<core::TraceEvent> scheduler_fixture(bool completion_checks = true) {
    auto events = fixture(false);
    for (auto e : fixture()) {
        if (e.name == "before") continue;
        e.ts += 300;
        if (e.name == "runtime.cpu_collective") {
            const auto index = e.arg_u64("collective_index") + 5;
            e.set_arg("collective_index", std::to_string(index));
            e.set_arg("sequence_before", std::to_string(index));
            e.set_arg("sequence_after", std::to_string(index + 1));
        }
        events.push_back(std::move(e));
    }
    for (int rank : { 0, 1 }) {
        const auto pid = std::to_string(rank + 1);
        for (const auto & [index, role] : std::map<int, std::string>{
                 { 1,        "request_receive" },
                 { 2, "write_completion_check" },
                 { 3,  "load_completion_check" },
                 { 4,  "storage_control_drain" }
        }) {
            const uint64_t at = 190 + 30 * index;
            events.push_back(event(index == 1 ? "c10d::broadcast_" : "c10d::allreduce_", at + 2, 3, pid, pid));
            events.push_back(event(index == 1 ? "gloo:broadcast" : "gloo:all_reduce", at + 4, 10, pid, "worker"));
            auto call = event("runtime.cpu_collective", at, 20, pid, pid);
            call.source_channel = core::TraceSourceChannel::PythonProbe;
            call.cat = "runtime_diagnostic";
            hicache_timing_fixture::set_collective_args(call, rank, index, "group");
            call.set_arg("operation", index == 1 ? "broadcast" : "all_reduce");
            call.set_arg("dtype", index == 1 ? "torch.int64" : "torch.int32");
            call.set_arg("numel", index == 4 ? "3" : "1");
            call.set_arg("group_src", "0");
            call.set_arg("role", role);
            events.push_back(std::move(call));
            if (completion_checks && (index == 2 || index == 3)) {
                auto check = event(index == 2 ? "runtime.hicache.write_completion" : "runtime.hicache.load_completion", at - 5, 30, pid, pid);
                check.source_channel = core::TraceSourceChannel::PythonProbe;
                check.cat = "runtime_diagnostic";
                check.set_arg("status", "returned");
                check.set_arg("blocking", "false");
                events.push_back(std::move(check));
            }
        }
        auto drain = event("runtime.hicache.storage_drain", 335, 5, pid, pid);
        drain.source_channel = core::TraceSourceChannel::PythonProbe;
        drain.cat = "runtime_diagnostic";
        for (const auto & [key, value] : std::map<std::string, std::string>{
                 {        "status", "returned" },
                 { "tp_world_size",        "2" },
                 {      "n_revoke",        "0" },
                 {      "n_backup",        "0" },
                 {     "n_release",        "0" }
        })
            drain.set_arg(key, value);
        events.push_back(std::move(drain));
    }
    return events;
}

void scheduler_body_excludes_both_progress_calls() {
    auto source_graph = build(scheduler_fixture());
    const hc::patch::HiCacheSourceDagIndex source(source_graph);
    const auto collectives = hc::observe_cpu_collectives(source);
    std::vector<hc::PrefetchControlObservation> previous, next;
    for (const auto & c : hc::observe_prefetch_control(source, collectives, "timeout"))
        (source.fact_node(c.progress_fact)->timestamp_us == 100 ? previous : next).push_back(c);
    const auto workers = hc::observe_prefetch_workers(source, collectives);
    const auto body = hc::model::observe_prefetch_scheduler_body(source, collectives, workers, previous, next);
    if (!body.issue.empty()) throw std::runtime_error("scheduler body fixture: " + body.issue);
    require(body.steps.size() == 4 && body.lanes.at(0).interval_start_us == 200 && body.lanes.at(0).interval_end_us == 400,
            "loop body must exclude the prior progress tail and next progress prefix");
    require(body.steps.back().after.at(0).observed_duration_us == 10 && body.tail.at(0).observed_duration_us == 60,
            "drain application and post-drain CPU must each be charged once");
    for (const auto i : { 1, 2 })
        require(body.steps[i].completion_observations.size() == 2 && body.steps[i].after.at(0).observed_duration_us == 5,
                "write/load application ends at the observed Python return, not MIN return");
    const auto portable = hc::model::prefetch_scheduler_timing(body, source_graph);
    for (const auto & [rank, tail] : portable.tail)
        require(tail.cpu_us == body.tail.at(rank).owned_node_duration_us && tail.gap_us == body.tail.at(rank).owned_gap_duration_us,
                "portable scheduler timing retains CPU and gap separately");
    std::map<int, std::vector<hc::PrefetchControlObservation>> grouped;
    for (const auto & call : previous) grouped[*call.rank].push_back(call);
    for (const auto & call : next) grouped[*call.rank].push_back(call);
    const auto wait_timing = hc::model::observe_prefetch_wait_timing(source, collectives, workers, grouped);
    require(wait_timing && wait_timing->scheduler.tail == portable.tail && wait_timing->scheduler.steps.size() == 4
                && wait_timing->check.cpu.at(0).max_to_stop.has_value(),
            "shared extractor supplies both retry and terminal branch work");
    require(!hc::model::observe_prefetch_wait_timing(source, collectives, workers, {}), "no observations do not fabricate an active wait sample");
    auto invalid_calls = grouped;
    invalid_calls.begin()->second.front().issue = "missing_prefetch_check";
    hicache_timing_fixture::require_throws<std::invalid_argument>(
        [&] { (void)hc::model::observe_prefetch_wait_timing(source, collectives, workers, invalid_calls); },
        "invalid active observations are errors, not absent calibration samples");
    require(portable.tail.at(0).cpu_us + portable.tail.at(0).gap_us == 60
                && portable.steps.back().after.at(0).cpu_us + portable.steps.back().after.at(0).gap_us == 10,
            "portable timing preserves measured tail and application costs");
    auto incomplete = body;
    incomplete.issue = "scheduler_body_missing_drain";
    hicache_timing_fixture::require_throws<std::invalid_argument>([&] { (void)hc::model::prefetch_scheduler_timing(incomplete, source_graph); },
                                                                  "portable conversion cannot hide incomplete observation evidence");
    const auto & tail_interval = body.tail.at(0);
    const auto * owner = source.fact_node(previous.front().progress_fact);
    source_graph.cpu_service_cost().add({ owner->pid, owner->tid }, { tail_interval.interval_start_us, tail_interval.interval_end_us, 30 });
    const auto corrected = hc::model::prefetch_scheduler_timing(body, source_graph);
    require(corrected.tail.at(0).cpu_us + corrected.tail.at(0).gap_us == 30 && corrected.tail.at(1) == portable.tail.at(1),
            "scheduler consumes the source service budget without changing the other rank");
    const auto & other_tail = body.tail.at(1);
    const auto other_call = std::ranges::find_if(previous, [](const auto & call) { return call.rank == 1; });
    const auto * other_owner = source.fact_node(other_call->progress_fact);
    // Only the first half is overridden: 30 original us become 60 service us.
    source_graph.cpu_service_cost().add({ other_owner->pid, other_owner->tid }, { other_tail.interval_start_us, other_tail.interval_start_us + 30, 60 });
    const auto increased = hc::model::prefetch_scheduler_timing(body, source_graph);
    require(increased.tail.at(1).cpu_us + increased.tail.at(1).gap_us == 90 && increased.tail.at(0) == corrected.tail.at(0)
                && increased.steps.front().before == portable.steps.front().before,
            "partial service spans can increase cost without changing unrelated scheduler intervals");
    for (const auto [late, extra_work] : std::vector<std::pair<uint64_t, uint64_t>>{
             {  0,  0 },
             { 20,  0 },
             {  0, 25 }
    }) {
        core::DagGraph graph;
        const auto root = graph.add_synthetic_node({ .name = "before scheduler body", .counts_toward_e2e = true });
        hc::CpuRankNodes entries;
        for (int rank : { 0, 1 }) {
            entries[rank] = graph.add_synthetic_node({ .name = "prior progress return", .duration = 200 + (rank ? late : 0) });
            graph.mutable_node(entries[rank]).gpu_id = rank;
            graph.add_edge(root, entries[rank], core::DagEdgeKind::Mutation);
        }
        hc::model::PrefetchSchedulerExecution execution;
        size_t actions = 0;
        (void)simulation::run_topological_simulation(graph, {}, [&](size_t node, uint64_t, simulation::FutureDag & future) {
            if (node == root) execution = hc::model::append_prefetch_scheduler_body(portable, entries, future);
            if (const auto at = execution.actions.find(node); at != execution.actions.end()) {
                ++actions; // Source timing audit, not target state prediction.
                const auto & event = at->second;
                if (extra_work && event.rank == 0 && event.apply && body.steps.at(event.step).action == hc::model::PrefetchSchedulerAction::WriteCompletion) {
                    const auto work = future.append({ .name = "explicit additional application work", .duration = extra_work });
                    future.depend(work, event.continuation);
                }
            }
        });
        require(actions == 16, "each of four steps exposes sample and apply on both ranks");
        uint64_t cpu = 0, gap = 0, expected_cpu = 0, expected_gap = 0;
        for (const auto & node : graph.nodes())
            if (graph.event_for_node(node.id).name == "hicache_scheduler_cpu") {
                cpu += node.duration;
                gap += node.cpu_gap_after;
            }
        const auto add = [&](const auto & times) {
            for (const auto & [rank, time] : times) {
                expected_cpu += time.cpu_us;
                expected_gap += time.gap_us;
            }
        };
        add(portable.tail);
        for (const auto & step : portable.steps) {
            add(step.before);
            add(step.after);
        }
        require(cpu == expected_cpu && gap == expected_gap, "scheduler graph must not relabel retained gaps as CPU work");
        for (const auto & [rank, node] : execution.returned)
            require(graph.node(node).completion_time == 400 + late + extra_work && graph.node(node).gpu_id == rank,
                    "ordinary loop work and communication must preserve source timing and late-peer propagation");
        hicache_timing_fixture::require_static_replay(graph, "materialized scheduler loop body must preserve execution times");
    }
    auto missing = workers;
    missing.drains.clear();
    require(hc::model::observe_prefetch_scheduler_body(source, collectives, missing, previous, next).issue == "scheduler_body_missing_drain",
            "a storage MIN cannot silently lose its application work");
    const auto inspect_body = [](std::vector<core::TraceEvent> events) {
        const auto graph = build(std::move(events));
        const hc::patch::HiCacheSourceDagIndex index(graph);
        const auto calls = hc::observe_cpu_collectives(index);
        std::vector<hc::PrefetchControlObservation> before, after;
        for (const auto & c : hc::observe_prefetch_control(index, calls, "timeout"))
            (index.fact_node(c.progress_fact)->timestamp_us == 100 ? before : after).push_back(c);
        return hc::model::observe_prefetch_scheduler_body(index, calls, hc::observe_prefetch_workers(index, calls), before, after);
    };
    const auto legacy = inspect_body(scheduler_fixture(false));
    require(legacy.issue.empty() && legacy.steps[1].completion_observations.empty() && legacy.steps[2].completion_observations.empty(),
            "old captures remain auditable but do not provide confirmation boundaries");
    for (const auto * invalid : { "raised", "blocking", "duplicate", "outside" }) {
        auto events = scheduler_fixture();
        auto check = std::ranges::find(events, "runtime.hicache.load_completion", &core::TraceEvent::name);
        if (std::string_view(invalid) == "raised") check->set_arg("status", "raised");
        else if (std::string_view(invalid) == "blocking") check->set_arg("blocking", "true");
        else if (std::string_view(invalid) == "outside") check->dur = 150;
        else {
            const auto copy = *check;
            events.push_back(copy);
        }
        require(!inspect_body(std::move(events)).issue.empty(), "ambiguous, blocking or incomplete confirmations cannot become ordinary loop work");
    }
}

void consecutive_waits_share_one_transaction() {
    auto events = scheduler_fixture();
    const auto first = events;
    for (auto e : first) {
        e.ts += 1'000;
        if (!e.arg("request_id").empty()) e.set_arg("request_id", "next-request");
        if (e.name == "runtime.cpu_collective")
            for (const auto * key : { "collective_index", "sequence_before", "sequence_after" }) e.set_arg(key, std::to_string(e.arg_u64(key) + 16));
        events.push_back(std::move(e));
    }
    for (const auto * pid : { "1", "2" }) {
        events.push_back(event("between requests", 600, 9, pid, pid));
        events.push_back(event("after both requests", 1'600, 2, pid, pid));
    }
    auto graph = build(std::move(events));
    const hc::patch::HiCacheSourceDagIndex source(graph);
    const auto rounds = hc::observe_cpu_collectives(source);
    const auto calls = hc::observe_prefetch_control(source, rounds, "timeout");
    const auto regions = hc::patch::prepare_prefetch_wait_regions(graph, calls, rounds, hc::observe_prefetch_workers(source, rounds));
    require(regions.size() == 4, "batched replacement keeps separate gates for both requests and ranks");
    for (const auto & round : rounds.rounds)
        for (const auto & call : round.calls)
            require(!graph.node(*call.worker).active && !graph.node(*call.submission).active, "both source waits must be removed in the same transaction");
    for (const auto & n : graph.nodes())
        if (graph.event_for_node(n.id).name == "between requests") require(n.active && n.duration == 9, "inter-request CPU must remain owned by neither wait");
    const auto root = graph.add_synthetic_node({ .name = "prepare both target waits" });
    (void)simulation::run_topological_simulation(graph, {}, [&](size_t node, uint64_t, simulation::FutureDag & future) {
        if (node != root) return;
        for (const auto & region : regions) {
            const auto work = future.append({ .name = "replacement wait", .duration = 100 });
            future.depend(region.entry, work);
            future.depend(work, region.exit);
        }
    });
    for (const auto & region : regions) {
        const auto first_request = source.fact_node(region.progress_fact)->request_id == "request";
        require(graph.node(region.entry).completion_time == (first_request ? 100 : 800),
                "second request must follow the first shortened wait and retained CPU/gaps");
        require(graph.node(region.exit).completion_time == graph.node(region.entry).completion_time + 100, "each replacement is charged once");
    }
    hicache_timing_fixture::require_static_replay(graph, "batched wait replacement preserves every node on static replay");
}

void whole_wait_follows_target_state() {
    for (const auto [read_us, expected_checks] : std::vector<std::pair<uint64_t, size_t>>{
             {  10, 1 },
             { 100, 2 },
             { 250, 3 }
    }) {
        auto events = scheduler_fixture();
        for (const auto * pid : { "1", "2" }) events.push_back(event("ordinary continuation", 600, 2, pid, pid));
        auto graph = build(events);
        const hc::patch::HiCacheSourceDagIndex source(graph);
        const auto collectives = hc::observe_cpu_collectives(source);
        const auto controls = hc::observe_prefetch_control(source, collectives, "wait_complete");
        const auto workers = hc::observe_prefetch_workers(source, collectives);
        std::vector<hc::PrefetchControlObservation> previous, next;
        for (const auto & c : controls) (c.stop ? next : previous).push_back(c);
        const auto body = hc::model::observe_prefetch_scheduler_body(source, collectives, workers, previous, next);
        hc::model::PrefetchCheckTiming timing;
        for (const auto & c : next) timing.cpu[*c.rank] = hc::model::observe_prefetch_check_cpu(c);
        for (const auto & c : previous) timing.cpu.at(*c.rank).max_to_false_return = hc::model::observe_prefetch_check_cpu(c).max_to_false_return;
        timing.state_max = hc::observe_cpu_collective_timing(graph, collectives.rounds.at(*previous.front().state_round));
        timing.completion_min = hc::observe_cpu_collective_timing(graph, collectives.rounds.at(*next.front().completion_round));
        const auto regions = hc::patch::prepare_prefetch_wait_regions(graph, controls, collectives, workers);
        require(regions.size() == 2, "whole wait must expose one entry/exit pair per rank");
        for (const auto & round : collectives.rounds)
            for (const auto & call : round.calls)
                require(!graph.node(*call.worker).active && !graph.node(*call.submission).active,
                        "whole wait replacement must remove old loop communication as well as progress calls");
        auto config = hicache_timing_fixture::timing_config();
        config.io_cost.storage_batch_pages = 2;
        config.io_cost.service_models.at("prefetch").stages = { static_cast<double>(read_us), 0, 5, 0, 10, 0 };
        hc::model::HiCacheState state(config);
        for (const int rank : { 0, 1 }) hicache_timing_fixture::seed_storage(state, "rank:" + std::to_string(rank) + ":cache");
        state.begin_formal_window(true);
        std::map<int, hc::HiCacheFact> facts;
        for (const int rank : { 0, 1 }) {
            auto fact = hicache_timing_fixture::request_fact("prefetch_candidate_anchor", "request", 200'000, 0);
            fact.cache_scope = "rank:" + std::to_string(rank) + ":cache";
            facts[rank] = fact;
            state.apply_fact(fact, hc::HiCacheFactRole::PrefetchCandidateAnchor);
            hicache_timing_fixture::complete_query(state, fact);
        }
        hc::model::HiCachePrefetchExecution worker(state, config);
        size_t actions = 0;
        hc::model::HiCachePrefetchWaitExecution waiting(
            state,
            facts,
            timing,
            hc::model::prefetch_scheduler_timing(body, graph),
            "wait_complete",
            [&](hc::model::PrefetchSchedulerAction, const hc::model::PrefetchSchedulerBoundary &, uint64_t, simulation::FutureDag &) {
                ++actions; // No receive/write/load operations exist in this synthetic state.
            });
        const auto root = graph.add_synthetic_node({ .name = "initialize target wait" });
        const auto result = simulation::run_topological_simulation(graph, {}, [&](size_t node, uint64_t at, simulation::FutureDag & future) {
            if (node == root) {
                for (const auto & [rank, fact] : facts) (void)worker.enqueue(fact, future);
                hc::CpuRankNodes entries;
                for (const auto & r : regions) entries[r.rank] = r.entry;
                const auto returned = waiting.start(entries, future);
                for (const auto & r : regions) future.depend(returned.at(r.rank), r.exit);
            }
            worker.advance(node, 200'000 + at, future);
            (void)waiting.advance(node, 200'000 + at, future);
            require(waiting.checks_issued() < 5, "target wait failed to make progress");
        });
        require(waiting.checks_issued() == expected_checks && actions == (expected_checks - 1) * 12,
                "target I/O must determine fewer, equal or more iterations than source");
        for (const auto & r : regions)
            require(graph.node(r.entry).completion_time == 100 && graph.node(r.exit).completion_time == 200 + (expected_checks - 1) * 300,
                    "old wait must be replaced exactly once, including its scheduler work");
        const auto replay = hicache_timing_fixture::require_static_replay(graph, "whole wait replay must preserve every node time");
        require(replay.e2e_us == result.e2e_us, "whole wait must survive static replay");
    }
}

void whole_wait_preserves_external_work() {
    for (const bool partial : { false, true }) {
        auto graph = build(scheduler_fixture());
        const hc::patch::HiCacheSourceDagIndex source(graph);
        const auto rounds = hc::observe_cpu_collectives(source);
        auto calls = hc::observe_prefetch_control(source, rounds, "wait_complete");
        const auto workers = hc::observe_prefetch_workers(source, rounds);
        size_t submit = 0;
        for (const auto & round : rounds.rounds)
            for (const auto & call : round.calls)
                if (graph.runtime_observations().at(call.observation).arg("role") == "write_completion_check") submit = *call.submission;
        if (partial) std::erase_if(calls, [](const auto & c) { return *c.rank == 1; });
        else {
            const auto consumer = graph.add_synthetic_node({ .name = "independent device consumer", .is_cpu = false, .duration = 1 });
            graph.add_edge(submit, consumer, core::DagEdgeKind::Mutation);
        }
        bool rejected = false;
        try {
            (void)hc::patch::prepare_prefetch_wait_regions(graph, calls, rounds, workers);
        }
        catch (const std::exception &) {
            rejected = true;
        }
        require(rejected && graph.node(submit).active, "whole wait cannot remove partial groups or independent loop consumers");
    }
}

std::vector<core::TraceEvent> service_fixture() {
    auto fact = event("service fact", 0, 60);
    fact.source_channel = core::TraceSourceChannel::PythonProbe;
    fact.set_arg("fact", R"({"class":"timing_observation","role":"storage_read_service_observed","consumers":["hicache_dag_patch"]})");
    fact.set_arg("phase", "end");
    fact.set_arg("source_page_size", "64");
    fact.set_arg("service_item_count", "2");
    std::vector<core::TraceEvent> events{ fact };
    for (int index = 0; index < 3; ++index) {
        auto e =
            event(index == 0 ? "runtime.hicache.prefetch_read" : "runtime.hicache.prefetch_publish", index == 0 ? 5 : 14 + 10 * index, index == 0 ? 10 : 2);
        e.source_channel = core::TraceSourceChannel::PythonProbe;
        e.cat = "runtime_diagnostic";
        e.set_arg("status", "returned");
        if (index == 0) e.set_arg("page_count", "2");
        else {
            e.set_arg("request_id", "request");
            e.set_arg("num_tokens", "64");
            e.set_arg("completed_tokens", std::to_string(64 * index));
            e.set_arg("accepted", "true");
        }
        events.push_back(std::move(e));
    }
    return events;
}

auto services(const std::vector<core::TraceEvent> & events) {
    const auto graph = build(events);
    return hc::observe_prefetch_services(hc::patch::HiCacheSourceDagIndex(graph));
}

auto service_ledger(std::vector<core::TraceEvent> events, std::optional<uint64_t> completed) {
    auto parent = events.front();
    parent.index = 100;
    parent.name = "prefetch worker";
    parent.dur = 80;
    parent.set_arg("fact", R"({"class":"timing_observation","role":"prefetch_io_observed","consumers":["hicache_dag_patch"]})");
    parent.set_arg("token_count", "128");
    if (completed) parent.set_arg("completed_token_count", std::to_string(*completed));
    events.push_back(std::move(parent));
    const auto graph = build(std::move(events));
    return hc::patch::build_hicache_io_operation_ledger(hc::patch::HiCacheSourceDagIndex(graph));
}
} // namespace

void check_prefetch_control_observation() {
    {
        std::vector<core::TraceEvent> events;
        for (const auto * name : { "runtime.hicache.prefetch_check",
                                   "runtime.hicache.prefetch_stop",
                                   "runtime.hicache.prefetch_publish",
                                   "runtime.hicache.prefetch_read",
                                   "runtime.hicache.prefetch_query",
                                   "runtime.hicache.host_release",
                                   "runtime.hicache.storage_drain",
                                   "runtime.hicache.prefetch_enqueue" }) {
            auto envelope = event(name, 10, 20);
            envelope.cat = "runtime_diagnostic";
            envelope.source_channel = core::TraceSourceChannel::PythonProbe;
            events.push_back(std::move(envelope));
        }

        const auto graph = build(std::move(events));
        require(graph.runtime_observations().size() == 8 && graph.node_count() == 0,
                "prefetch timing envelopes are observations, not additional executable cost");
    }

    {
        const auto graph = build(with_local_returns());
        const hc::patch::HiCacheSourceDagIndex source(graph);
        const auto rounds = hc::observe_cpu_collectives(source);
        const auto all = hc::observe_prefetch_requests(source, rounds, "timeout");
        const std::set<std::string> none, active{ "request" };
        require(all.size() == 2 && all.at("request").size() == 2, "shared observation retains request and rank identities");
        require(hc::observe_prefetch_requests(source, rounds, "timeout", &active).size() == 1, "runtime filter excludes requests outside its HTTP window");
        require(hc::observe_prefetch_requests(source, rounds, "unsupported", &none).empty(),
                "an empty request set selects none and does not validate unrelated calls");
    }
    consecutive_waits_share_one_transaction();
    {
        auto control = event("hicache.control.prefetch", 100, 100);
        control.source_channel = core::TraceSourceChannel::PythonProbe;
        const auto leaf = event("actual instruction", 130, 20);
        auto enqueue = event("runtime.hicache.prefetch_enqueue", 110, 50);
        enqueue.source_channel = core::TraceSourceChannel::PythonProbe;
        enqueue.cat = "runtime_diagnostic";
        auto plain = build({ control, leaf });
        auto split = build({ control, leaf, enqueue });
        const hc::patch::HiCacheSourceDagIndex index(split);
        require(index.cpu_node_starting_at("1", "1", 110).has_value() && index.cpu_node_starting_at("1", "1", 160).has_value(),
                "enqueue endpoints must partition generated control self time");
        uint64_t self = 0;
        size_t leaves = 0;
        for (const auto & node : split.nodes()) {
            const auto & e = split.event_for_node(node.id);
            if (e.name.ends_with(".self")) self += node.duration;
            if (e.name == leaf.name) {
                ++leaves;
                require(e.ts == 130 && node.duration == 20, "enqueue boundaries must not split actual CPU instructions");
            }
        }
        require(self == 80 && leaves == 1, "enqueue partition must neither add nor remove CPU time");
        require(simulation::run_topological_simulation(plain).e2e_us == simulation::run_topological_simulation(split).e2e_us,
                "enqueue partition alone must preserve completion");
        const hc::patch::HiCacheSourceDagIndex original(plain);
        const auto & before = plain.node(*original.cpu_node_starting_at("1", "1", 130));
        const auto & after = split.node(*index.cpu_node_starting_at("1", "1", 130));
        require(before.simulation_start == after.simulation_start && before.completion_time == after.completion_time,
                "enqueue partition must preserve the actual instruction's start and completion");
    }
    scheduler_body_excludes_both_progress_calls();
    whole_wait_follows_target_state();
    whole_wait_preserves_external_work();
    {
        auto events = with_local_returns();
        auto graph = build(events);
        const auto calls = observe(graph);
        const hc::patch::HiCacheSourceDagIndex service_source(graph);
        size_t locals = 0;
        for (const auto & call : calls) {
            if (!call.issue.empty()) throw std::runtime_error("inactive call observation: " + call.issue);
            if (!call.local_return) continue;
            ++locals;
            require(!call.check && !call.stop && !call.state_round && !call.completion_round && call.rank && call.cpu.size() == 1
                        && call.cpu.front().role == "local_return" && call.cpu.front().cpu.observed_duration_us == 7,
                    "local return retains the whole measured cost without communication");
            require(hc::model::observe_prefetch_check_cpu(call, &service_source).no_operation_return == 7, "empty service keeps observed local return cost");
            const auto * fact = service_source.fact_node(call.progress_fact);
            const auto & span = call.cpu.front().cpu;
            graph.cpu_service_cost().add({ fact->pid, fact->tid }, { span.interval_start_us, span.interval_end_us, 3 });
            require(hc::model::observe_prefetch_check_cpu(call, &service_source).no_operation_return == 3,
                    "local return template must consume existing normal service");
            require(hc::model::observe_prefetch_check_cpu(call).no_operation_return == 7 && call.cpu.front().cpu.observed_duration_us == 7,
                    "service extraction must not change raw calibration or removal ownership");
        }
        require(locals == 2, "both inactive rank calls must be recognized");
        auto missing = events;
        std::erase_if(missing, [](const auto & e) { return e.name == "runtime.hicache.prefetch_check"; });
        const auto unobserved = observe(build(missing));
        require(std::ranges::all_of(unobserved, [](const auto & c) { return c.issue == "missing_prefetch_check"; }),
                "missing probes cannot prove inactive execution");
        // No active prefetch check anywhere in this source. Other collectives
        // still establish rank identity, but do not prove the local branch.
        for (const auto & e : events) {
            if (e.name != "progress fact" || e.ts != 300) continue;
            auto proof = event("runtime.hicache.prefetch_progress", 300, 7, e.pid, e.tid);
            proof.source_channel = core::TraceSourceChannel::PythonProbe;
            proof.cat = "runtime_diagnostic";
            proof.set_arg("request_id", "no-operation");
            proof.set_arg("status", "returned");
            proof.set_arg("progress_ready", "true");
            proof.set_arg("entry_branch", e.pid == "1" ? "no_operation" : "host_not_allocated");
            missing.push_back(std::move(proof));
        }
        const auto proven = observe(build(missing));
        require(std::ranges::count_if(proven, [](const auto & c) { return c.local_return && c.issue.empty(); }) == 2,
                "explicit entry evidence proves local returns without any active check sample");
        for (auto & e : missing)
            if (e.name == "runtime.hicache.prefetch_progress") e.set_arg("entry_branch", "active");
        const auto inconsistent = observe(build(missing));
        require(std::ranges::count(inconsistent, "unproven_prefetch_local_return", &hc::PrefetchControlObservation::issue) == 2,
                "an active entry with no check must not become a local return");
        for (auto & e : events)
            if (e.name == "progress fact" && e.ts == 300) e.set_arg("progress_ready", "false");
        const auto not_ready = observe(build(events));
        require(std::ranges::count(not_ready, "unproven_prefetch_local_return", &hc::PrefetchControlObservation::issue) == 2,
                "a failed return without a check is not an inactive path");
    }
    for (const bool source_done : { false, true })
        for (const int target : { 0, 1, 2 }) {
            const bool done = target == 1, inactive = target == 2;
            auto events = fixture(source_done);
            for (const auto * pid : { "1", "2" }) {
                events.push_back(event("aten::to", 108, 0, pid, pid));
                events.push_back(event("aten::to", 200, 0, pid, pid));
            }
            for (const auto * pid : { "1", "2" }) events.push_back(event("ordinary after control", 210, 2, pid, pid));
            const auto source_events = events;
            for (const auto & original : source_events) {
                if (original.name != "runtime.cpu_collective" || original.arg("role") != "prefetch_state_check") continue;
                auto call = original;
                call.ts = 220;
                call.set_arg("collective_index", "2");
                call.set_arg("role", "ordinary");
                events.push_back(call);
                events.push_back(event("c10d::allreduce_", 222, 3, call.pid, call.tid));
                events.push_back(event("gloo:all_reduce", 224, 10, call.pid, "worker"));
                events.push_back(event("ordinary continuation", 300, 2, call.pid, call.tid));
            }
            auto replaced = build(events);
            const auto baseline = simulation::run_topological_simulation(replaced).e2e_us;
            // Build a fresh graph: preparation is before queue materialization.
            replaced = build(events);
            const hc::patch::HiCacheSourceDagIndex source(replaced);
            const auto collectives = hc::observe_cpu_collectives(source);
            const auto controls = hc::observe_prefetch_control(source, collectives, "timeout");
            const auto regions = hc::patch::prepare_prefetch_control_regions(replaced, controls, collectives);
            require(regions.size() == 2, "both rank calls must expose replacement gates");
            auto config = hicache_timing_fixture::timing_config();
            config.io_cost.storage_batch_pages = 2;
            config.io_cost.service_models.at("prefetch").stages = { 10, 0, 5, 0, 10, 0 };
            hc::model::HiCacheState state(config);
            for (int rank : { 0, 1 }) hicache_timing_fixture::seed_storage(state, "rank:" + std::to_string(rank) + ":cache");
            state.begin_formal_window(true);
            std::map<int, hc::HiCacheFact> facts;
            hc::model::PrefetchCheckTiming timing;
            for (const auto & c : controls) {
                auto fact = hicache_timing_fixture::request_fact("prefetch_candidate_anchor", "request", 200'000, 0);
                fact.cache_scope = "rank:" + std::to_string(*c.rank) + ":cache";
                facts[*c.rank] = fact;
                if (!inactive) state.apply_fact(fact, hc::HiCacheFactRole::PrefetchCandidateAnchor);
                if (done) hicache_timing_fixture::complete_query(state, fact);
                timing.cpu[*c.rank] = hc::model::observe_prefetch_check_cpu(c);
            }
            timing.state_max = hc::observe_cpu_collective_timing(source.graph(), collectives.rounds.at(*controls[0].state_round));
            // A false-only fixture never executes its terminal branch. Supply the
            // separately observed successful call, rather than invent MIN timing.
            const auto terminal_source = build(fixture());
            const hc::patch::HiCacheSourceDagIndex terminal_index(terminal_source);
            const auto terminal_rounds = hc::observe_cpu_collectives(terminal_index);
            const auto terminal_controls = hc::observe_prefetch_control(terminal_index, terminal_rounds, "timeout");
            timing.completion_min = hc::observe_cpu_collective_timing(terminal_source, terminal_rounds.rounds.at(*terminal_controls[0].completion_round));
            for (const auto & c : terminal_controls) {
                auto & cpu = timing.cpu.at(*c.rank);
                const auto measured = hc::model::observe_prefetch_check_cpu(c);
                cpu.max_to_stop = measured.max_to_stop;
                cpu.stop_to_min = measured.stop_to_min;
                cpu.min_to_visible = measured.min_to_visible;
            }
            const auto retry_source = build(fixture(false));
            for (const auto & c : observe(retry_source))
                timing.cpu.at(*c.rank).max_to_false_return = hc::model::observe_prefetch_check_cpu(c).max_to_false_return;
            const auto inactive_source = build(with_local_returns());
            for (const auto & c : observe(inactive_source))
                if (c.local_return) timing.cpu.at(*c.rank).no_operation_return = hc::model::observe_prefetch_check_cpu(c).no_operation_return;
            hc::model::HiCachePrefetchCheckExecution check(state, facts, timing, "wait_complete");
            hc::model::HiCachePrefetchExecution worker(state, config);
            const auto root = replaced.add_synthetic_node({ .name = "before execution" });
            (void)simulation::run_topological_simulation(replaced, {}, [&](size_t node, uint64_t at, simulation::FutureDag & future) {
                if (node == root) {
                    for (const auto & [rank, fact] : facts)
                        if (done) (void)worker.enqueue(fact, future);
                    hc::CpuRankNodes entries;
                    for (const auto & r : regions) entries[r.rank] = r.entry;
                    const auto returned = check.start(entries, future);
                    for (const auto & r : regions) future.depend(returned.at(r.rank), r.exit);
                }
                worker.advance(node, 200'000 + at, future);
                if (const auto ready = check.advance(node, 200'000 + at, future))
                    require(*ready == (done || inactive), "replacement branch must follow actual target state");
            });
            const uint64_t reduction = inactive ? 93 : 0;
            require(replaced.node(regions[0].entry).completion_time == 100 && replaced.node(regions[0].exit).completion_time == 200 - reduction,
                    "replacement must execute once between original entry and exit");
            require(simulation::run_topological_simulation(replaced).e2e_us == baseline - reduction, "outer CPU and gaps must survive control replacement");
            for (const auto & round : collectives.rounds)
                for (const auto & call : round.calls) {
                    if (replaced.runtime_observations()[call.observation].arg("role") == "ordinary") {
                        require(replaced.node(*call.worker).active && replaced.node(*call.worker).completion_time == 234 - reduction,
                                "retained worker must use its own arrival, not the removed worker's idle gap");
                        continue;
                    }
                    require(!replaced.node(*call.worker).active && !replaced.node(*call.submission).active,
                            "old communication work must be removed, not charged twice");
                }
        }
    for (const bool partial_group : { true, false }) {
        auto events = fixture();
        for (const auto * pid : { "1", "2" }) events.push_back(event("after", 210, 2, pid, pid));
        auto graph = build(events);
        const hc::patch::HiCacheSourceDagIndex source(graph);
        const auto rounds = hc::observe_cpu_collectives(source);
        auto calls = hc::observe_prefetch_control(source, rounds, "timeout");
        const auto submit = *rounds.rounds[0].calls[0].submission;
        if (partial_group) calls.pop_back();
        else {
            const auto consumer = graph.add_synthetic_node({ .name = "independent device consumer", .is_cpu = false, .duration = 1 });
            graph.add_edge(submit, consumer, core::DagEdgeKind::Correlation);
        }
        bool rejected = false;
        try {
            (void)hc::patch::prepare_prefetch_control_regions(graph, calls, rounds);
        }
        catch (const std::exception &) {
            rejected = true;
        }
        require(rejected && graph.node(submit).active, "partial groups and independent consumers must not be silently removed");
    }
    auto adjacent = fixture();
    for (auto & event : adjacent)
        if (event.name == "before") event.dur = 100;
    auto adjacent_graph = build(std::move(adjacent));
    for (const auto & call : observe(adjacent_graph)) { require(call.issue.empty(), "an exactly adjacent predecessor must not hide the control interval"); }
    const hc::patch::HiCacheSourceDagIndex adjacent_index(adjacent_graph);
    require(adjacent_index.cpu_node_starting_at("1", "1", 105).has_value(), "partitioned check starts must be exact execution boundaries");
    require(!adjacent_index.cpu_node_starting_at("1", "1", 106), "a point inside a leaf must not become a fabricated boundary");
    require(!adjacent_index.cpu_node_starting_at("1", "missing", 105), "another thread is not an execution boundary fallback");
    auto duplicate = adjacent_graph.event_for_node(*adjacent_index.cpu_node_starting_at("1", "1", 105));
    auto & event_list = adjacent_graph.mutable_events();
    event_list.push_back(std::move(duplicate));
    (void)adjacent_graph.add_node(event_list.size() - 1, true, "duplicate");
    require(!hc::patch::HiCacheSourceDagIndex(adjacent_graph).cpu_node_starting_at("1", "1", 105),
            "two starts on the same observed thread need disambiguation, not a node-id tie break");
    auto graph = build(fixture());
    auto results = observe(graph);
    require(results.size() == 2, "each rank retains its own progress observation");
    for (const auto & result : results) {
        if (!result.issue.empty()) throw std::runtime_error("prefetch fixture: " + result.issue);
        require(result.issue.empty() && result.state_round && result.completion_round && result.completed_tokens == 128,
                "true progress binds check, MAX, stop and MIN");
        const auto measured = hc::model::observe_prefetch_check_cpu(result);
        require(measured.check_to_max == 5 && measured.max_to_stop == 15 && measured.stop_to_min == 15 && measured.min_to_visible == 20
                    && measured.entry_to_check == 5 && !measured.max_to_false_return && !measured.no_operation_return,
                "a terminal observation supplies terminal costs, not its unobserved false or inactive branches");
        uint64_t duration = 0;
        for (const auto & interval : result.cpu) duration += interval.cpu.observed_duration_us;
        require(duration == 60, "control intervals plus MAX/MIN exactly partition the progress call");
        const auto stop = std::ranges::find(result.cpu, "stop_call", &hc::PrefetchControlCpuInterval::role);
        require(stop != result.cpu.end() && stop->cpu.interval_start_us == 145 && stop->cpu.interval_end_us == 150,
                "stop observation preserves cancellation/snapshot uncertainty");
    }
    uint64_t cpu = 0;
    for (const auto & node : graph.nodes()) {
        const auto & e = graph.event_for_node(node.id);
        if (node.is_cpu && e.pid == e.tid && e.name != "before") cpu += e.dur;
    }
    require(cpu == 200, "splitting generated control remainders does not add or remove CPU cost");
    auto without_boundaries = fixture();
    std::erase_if(without_boundaries, [](const auto & e) { return e.name.starts_with("runtime.hicache.prefetch_"); });
    auto unsplit_graph = build(without_boundaries);
    require(simulation::run_topological_simulation(graph).e2e_us == simulation::run_topological_simulation(unsplit_graph).e2e_us,
            "control segmentation preserves same-cost DAG replay time");
    results = observe(build(fixture(false)));
    require(results.size() == 2 && results[0].issue.empty() && !results[0].stop && !results[0].completion_round,
            "false progress retains MAX and CPU work without inventing stop/MIN");
    const auto retry_cpu = hc::model::observe_prefetch_check_cpu(results[0]);
    require(retry_cpu.max_to_false_return == 70 && !retry_cpu.max_to_stop && !retry_cpu.stop_to_min && !retry_cpu.min_to_visible,
            "a retry observation must not silently provide zero terminal cost");
    results = observe(build(fixture(true, true)), "best_effort");
    require(results.size() == 2 && results[0].issue.empty() && !results[0].state_round && results[0].completion_round,
            "known best-effort source skips MAX but retains stop and MIN");
    require(observe(build(fixture(true, true)))[0].issue == "prefetch_collectives_do_not_match_policy_or_decision", "missing MAX does not imply best-effort");
    require(observe(graph, "")[0].issue == "missing_or_unsupported_source_policy", "source policy cannot be guessed");
    auto events = fixture();
    for (auto & e : events)
        if (e.name == "progress fact") {
            e.ts += 2;
            e.dur -= 4;
        }
    results = observe(build(events));
    require(results[0].issue.empty() && results[0].uses_control_envelope && results[0].cpu.front().cpu.interval_start_us == 100
                && results[0].cpu.back().cpu.interval_end_us == 200,
            "a narrow semantic fact does not drop probe work in the enclosing CPU call");
    events = fixture();
    for (auto & e : events)
        if (e.name == "runtime.hicache.prefetch_stop") e.dur = 0;
    results = observe(build(events));
    require(results[0].issue.empty(), "an explicitly observed zero-duration stop still defines a CPU partition boundary");
    events = fixture();
    std::erase_if(events, [](const auto & e) { return e.name == "runtime.hicache.prefetch_check"; });
    require(observe(build(events))[0].issue == "missing_prefetch_check", "old traces do not acquire invented check costs");
    events = fixture();
    for (auto & e : events)
        if (e.name == "runtime.hicache.prefetch_stop") e.set_arg("request_id", "another");
    require(observe(build(events))[0].issue == "prefetch_boundary_identity_or_status", "another request cannot supply stop costs");
    events = fixture();
    for (auto & e : events)
        if (e.name == "runtime.hicache.prefetch_check") e.set_arg("can_terminate", "false");
    require(observe(build(events))[0].issue == "prefetch_decision_not_observed_or_inconsistent", "check and progress must agree");
    events = fixture();
    events.push_back(event("real_cpu_leaf", 143, 9));
    require(observe(build(events))[0].issue == "prefetch_cpu_interval_incomplete", "observation boundaries never split a real CPU leaf");

    auto reads = service_fixture();
    auto measured = services(reads);
    require(measured.size() == 1 && measured[0].issue.empty() && measured[0].request_id == "request", "service identity comes from its contained increments");
    const auto & batch = measured[0].batch;
    require(batch.page_count == 2 && batch.start_ts == 0 && batch.ready_ts == 60 && batch.publications.size() == 2 && batch.publications[0].at_us == 26
                && batch.publications[1].at_us == 36 && !batch.publications[0].cancelled_return_us,
            "source service retains pagewise publication upper bounds and total cost without inventing a cancelled return");
    require(measured[0].observed_work && measured[0].observed_work->copied_pages == 2 && measured[0].observed_work->published_pages == 2,
            "successful service records all physical copies and publications");
    reads[1].tid = "other-worker";
    require(services(reads)[0].issue == "missing_prefetch_read_or_publications", "an unrelated worker cannot supply the batch read");
    reads = service_fixture();
    reads.push_back(reads[1]);
    require(services(reads)[0].issue == "nonunique_prefetch_read", "ambiguous read boundaries are not guessed");
    reads = service_fixture();
    reads.back().set_arg("accepted", "false");
    reads.back().set_arg("completed_tokens", "64");
    measured = services(reads);
    require(measured[0].issue == "prefetch_service_not_fully_published" && measured[0].batch.publications.empty(),
            "cancelled source work does not provide a full hypothetical target service");
    require(measured[0].observed_work && measured[0].observed_work->copied_pages == 2 && measured[0].observed_work->published_pages == 1,
            "the rejected publication still paid its physical copy");
    auto ledger = service_ledger(reads, 64);
    require(ledger.records.size() == 1 && ledger.records[0].storage_service_batches.size() == 1
                && ledger.records[0].storage_service_batches[0].copied_page_count == 2
                && ledger.records[0].storage_service_batches[0].published_page_count == 1,
            "source operation ledger carries measured cancelled work to the model builder");
    reads.erase(reads.begin() + 2);
    ledger = service_ledger(reads, 64);
    require(!ledger.records[0].storage_service_batches[0].copied_page_count,
            "missing early publications cannot undercount copies when the worker completed counter proves them");
    reads.back().set_arg("completed_tokens", "0");
    measured = services(reads);
    require(measured[0].observed_work && measured[0].observed_work->copied_pages == 1 && measured[0].observed_work->published_pages == 0,
            "cancellation before the first publication still copies one page");
    reads.push_back(reads.back());
    require(!services(reads)[0].observed_work, "no publication may follow a rejected increment");
    reads = service_fixture();
    reads.resize(1);
    ledger = service_ledger(reads, 128);
    require(ledger.records[0].storage_service_batches[0].copied_page_count == 2,
            "an explicit full worker-local completed count proves copying without per-page timings");
    require(!service_ledger(reads, 64).records[0].storage_service_batches[0].copied_page_count
                && !service_ledger(reads, std::nullopt).records[0].storage_service_batches[0].copied_page_count,
            "partial or inferred completed counters do not prove copy work");
    reads = service_fixture();
    reads.back().set_arg("request_id", "other-request");
    require(services(reads)[0].issue == "prefetch_publication_identity_or_status", "one service cannot combine different requests");
    reads = service_fixture();
    reads.back().set_arg("completed_tokens", "256");
    require(services(reads)[0].issue == "prefetch_publication_counter_discontinuity", "measured publication counters must advance by the supplied tokens");
    require(!service_ledger(reads, 128).records[0].storage_service_batches[0].copied_page_count,
            "an aggregate completed counter cannot override contradictory page observations");
    reads = service_fixture();
    reads.back().ts = 10;
    require(services(reads)[0].issue == "prefetch_publication_order_or_geometry", "host publication cannot precede the backend read");
    reads = service_fixture();
    reads.pop_back();
    require(services(reads)[0].issue == "prefetch_service_not_fully_published" && !services(reads)[0].observed_work,
            "missing pages are not filled by uniformly splitting total service time");
    reads = service_fixture();
    auto duplicate_service = reads.front();
    duplicate_service.index = 1;
    reads.push_back(duplicate_service);
    measured = services(reads);
    require(measured.size() == 2
                && std::ranges::all_of(
                    measured,
                    [](const auto & service) { return service.issue == "nonunique_prefetch_service_parent" && service.batch.publications.empty(); }),
            "overlapping service parents cannot count one physical transfer twice");
}
