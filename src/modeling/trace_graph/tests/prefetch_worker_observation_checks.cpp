#include "markov/trace_graph/core/dag_builder.hpp"
#include "markov/trace_graph/modules/hicache/prefetch_worker_observation.hpp"
#include "markov/trace_graph/modules/hicache/runtime/prefetch_queries.hpp"
#include "markov/trace_graph/modules/hicache/runtime/queue_confirmations.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_confirmations.hpp"
#include "markov/trace_graph/simulation/topological_simulator.hpp"
#include "hicache_timing_fixture.hpp"
#include <algorithm>
#include <stdexcept>
#include <fstream>
#include <filesystem>
#include <unistd.h>
#include <nlohmann/json.hpp>

using namespace markov::trace_graph;
namespace hc = modules::hicache;
namespace {
using hicache_timing_fixture::require;

std::vector<core::TraceEvent> fixture(bool single = false) {
    std::vector<core::TraceEvent> events;
    for (int rank = 0; rank < (single ? 1 : 2); ++rank) {
        const auto pid = std::to_string(rank + 1);
        const auto add =
            [&](const char * name, const char * tid, uint64_t ts, uint64_t duration, std::initializer_list<std::pair<const char *, std::string>> args)
            -> core::TraceEvent & {
            core::TraceEvent e;
            e.index = events.size();
            e.name = name;
            e.pid = pid;
            e.tid = tid;
            e.ts = ts;
            e.dur = duration;
            e.source_channel = core::TraceSourceChannel::PythonProbe;
            e.cat = "runtime_diagnostic";
            e.set_arg("status", "returned");
            for (const auto & [key, value] : args) e.set_arg(key, value);
            events.push_back(std::move(e));
            return events.back();
        };
        auto & candidate = add("hicache_candidate",
                               "main",
                               5,
                               10,
                               {
                                   { "request_id",                                                                                                       "r" },
                                   {      "phase",                                                                                                     "end" },
                                   {       "fact", R"({"class":"workload_identity","role":"prefetch_candidate_anchor","consumers":["hicache_state_model"]})" }
        });
        candidate.cat = "hicache";
        add("runtime.hicache.prefetch_enqueue",
            "main",
            8,
            4,
            {
                {       "request_id",   "r" },
                {     "operation_id",  "42" },
                {        "page_size",  "64" },
                { "allocated_tokens", "192" }
        });
        add("runtime.hicache.prefetch_query",
            "query",
            10,
            10,
            {
                {       "request_id",                "r" },
                {     "operation_id",               "42" },
                {        "page_size",               "64" },
                { "local_hit_tokens",              "128" },
                { "sync_group_count", single ? "0" : "1" }
        });
        const auto collective = [&](const char * group, const char * role, const char * tid, uint64_t ts, const char * numel) {
            auto & call = add("runtime.cpu_collective", tid, ts, 2, {});
            hicache_timing_fixture::set_collective_args(call, rank, 0, group);
            call.set_arg("numel", numel);
            call.set_arg("dtype", "int32");
            call.set_arg("role", role);
        };
        if (!single) collective("query-group", "prefetch_storage_hit_agreement", "query", 22, "1");
        add("runtime.hicache.host_release",
            "query",
            25,
            1,
            {
                { "token_count", "64" },
                {   "page_size", "64" }
        });
        auto & worker = add("hicache_worker",
                            "worker",
                            30,
                            50,
                            {
                                {            "request_id",                                                                                                 "r" },
                                {          "operation_id",                                                                                                "42" },
                                {      "source_page_size",                                                                                                "64" },
                                { "completed_token_count",                                                                                                "64" },
                                {           "page_hashes",                                                                                      R"(["a","b"])" },
                                {                 "phase",                                                                                               "end" },
                                {                  "fact", R"({"class":"timing_observation","role":"prefetch_io_observed","consumers":["hicache_dag_patch"]})" }
        });
        worker.cat = "hicache";
        add("runtime.hicache.host_release",
            "worker",
            82,
            1,
            {
                { "token_count", "64" },
                {   "page_size", "64" }
        });
        auto & progress = add("hicache_progress",
                              "main",
                              84,
                              10,
                              {
                                  {     "request_id",                                                                                                  "r" },
                                  {          "phase",                                                                                                "end" },
                                  { "progress_ready",                                                                                               "true" },
                                  {           "fact", R"({"class":"source_actual","role":"prefetch_progress_observed","consumers":["hicache_dag_patch"]})" }
        });
        progress.cat = "hicache";
        add("runtime.hicache.host_release",
            "main",
            88,
            1,
            {
                { "token_count",                   "0" },
                {   "page_size",                  "64" },
                {        "role", "prefetch_completion" },
                {  "request_id",                   "r" }
        });
        if (!single) collective("drain-group", "storage_control_drain", "main", 100, "3");
        add("runtime.hicache.storage_drain",
            "main",
            105,
            1,
            {
                {      "n_revoke",                "0" },
                {      "n_backup",                "0" },
                {     "n_release",                "2" },
                { "tp_world_size", single ? "1" : "2" }
        });
    }
    return events;
}
hc::PrefetchWorkerObservations inspect(std::vector<core::TraceEvent> events) {
    const auto graph = core::DagBuilder(1).build(std::move(events), 0);
    const hc::patch::HiCacheSourceDagIndex source(graph);
    return hc::observe_prefetch_workers(source, hc::observe_cpu_collectives(source));
}
void check_issue(const hc::PrefetchWorkerObservations & result, const char * issue) { require(result.issues.contains(issue), issue); }

void ordinary_write_confirmations(bool with_work = false) {
    // Two ranks: first DMA completes at 20/30, second at 70 on both.
    // MIN samples at 25/55/85; wrapper returns at 45/75/105.
    std::vector<core::TraceEvent> events, identities;
    for (int rank = 0; rank < 2; ++rank) {
        const auto pid = std::to_string(rank + 1);
        const auto add = [&](std::string name, std::string tid, uint64_t ts, uint64_t dur) -> core::TraceEvent & {
            core::TraceEvent event;
            event.index = events.size();
            event.name = std::move(name);
            event.pid = pid;
            event.tid = std::move(tid);
            event.ts = ts;
            event.dur = dur;
            event.cat = "cpu_op";
            event.source_channel = core::TraceSourceChannel::Torch;
            events.push_back(std::move(event));
            return events.back();
        };
        add("before", pid, 0, 1);
        add("second_write", pid, 50, 0);
        for (int round = 0; round < 3; ++round) {
            const uint64_t at = 25 + round * 30;
            add("c10d::allreduce_", pid, at + 2, 5);
            add("gloo:all_reduce", "worker", at + 3, 10);
            auto & call = add("runtime.cpu_collective", pid, at, 14);
            call.cat = "runtime_diagnostic";
            call.source_channel = core::TraceSourceChannel::PythonProbe;
            for (const auto & [key, value] : std::map<std::string, std::string>{
                     {"group", "write"}, {"members", "[0,1]"}, {"rank", std::to_string(rank)},
                     {"collective_index", std::to_string(round)}, {"sequence_before", std::to_string(round)},
                     {"sequence_after", std::to_string(round + 1)}, {"operation", "all_reduce"},
                     {"reduce_op", "MIN"}, {"numel", "1"}, {"dtype", "torch.int32"},
                     {"async_op", "false"}, {"status", "returned"}, {"role", "write_completion_check"}})
                call.set_arg(key, value);
            auto & wrapper = add("runtime.hicache.write_completion", pid, at - 1, 21);
            wrapper.cat = "runtime_diagnostic";
            wrapper.source_channel = core::TraceSourceChannel::PythonProbe;
            wrapper.set_arg("status", "returned");
            wrapper.set_arg("blocking", "false");
        }
        add("after", pid, 120, 1);
        core::TraceEvent identity;
        identity.name = "hicache_commit";
        identity.index = rank;
        identity.pid = identity.tid = pid;
        identity.ts = 1;
        identity.source_channel = core::TraceSourceChannel::PythonProbe;
        for (const auto & [key, value] : std::map<std::string, std::string>{
                 {"fact", R"({"class":"workload_identity","role":"cache_lifecycle_commit","consumers":["hicache_state_model"]})"},
                 {"phase", "end"}, {"request_id", "identity"}, {"seq_no", "1"}, {"cache_scope", "cache:" + pid},
                 {"lifecycle_kind", "finished"}, {"token_count", "4"},
                 {"full_path_span", R"({"path_id":"p","begin":0,"end":4,"token_count":4})"},
                 {"token_dictionary", R"({"path_id":"p","token_ids":[1,2,3,4]})"}})
            identity.set_arg(key, value);
        identities.push_back(std::move(identity));
    }
    auto graph = core::DagBuilder(1).build(std::move(events), 0);
    graph.set_hicache_fact_events(identities);
    auto config = hicache_timing_fixture::timing_config();
    config.l1_capacity_pages = 16;
    config.l2_capacity_pages = 32;
    {
        auto loads = graph;
        auto observations = loads.runtime_observations();
        for (auto & event : observations) {
            if (event.name == "runtime.hicache.write_completion") event.name = "runtime.hicache.load_completion";
            if (event.arg("role") == "write_completion_check") event.set_arg("role", "load_completion_check");
        }
        loads.set_runtime_observations(std::move(observations));
        hc::model::HiCacheModelReplay empty(loads, config, true);
        hc::runtime::HiCacheQueueConfirmations confirmations;
        size_t returns = 0;
        confirmations.bind(loads, empty, 0, 121, {}, [&](const auto & result) {
            require(!result.write && !result.storage && !result.confirmed_loads,
                    "generated empty load confirmation uses target queue state");
            ++returns;
        }, {}, std::nullopt, true);
        confirmations.replace_load_tails(loads);
        (void)simulation::run_topological_simulation(loads, {}, [&](size_t node, uint64_t time, simulation::FutureDag & future) {
            confirmations.advance(node, time, future);
        });
        require(returns == 6, "generated load tails preserve all rank returns");
        const auto executed = loads.nodes();
        (void)simulation::run_topological_simulation(loads);
        for (const auto & node : executed)
            if (node.active) require(loads.node(node.id).completion_time == node.completion_time,
                                     "generated load confirmation is a replayable DAG, not only a state callback");
    }
    hc::model::HiCacheModelReplay replay(graph, config, true);
    require(replay.facts().size() == 2, "write fixture requires both cache identities");
    auto backup = [&](const std::string & pid, uint64_t at, uint32_t token) {
        auto fact = hicache_timing_fixture::request_fact("cache_lifecycle_commit", std::to_string(token), at, token);
        fact.pid = fact.tid = pid;
        fact.cache_scope = "cache:" + pid;
        fact.lifecycle_kind = "finished";
        replay.state().apply_fact(fact, hc::HiCacheFactRole::CacheLifecycleCommit);
        return fact;
    };
    for (int rank = 0; rank < 2; ++rank) backup(std::to_string(rank + 1), rank * 10, 0);
    {
        auto blocking = graph;
        auto observations = blocking.runtime_observations();
        for (auto & event : observations)
            if (event.name == "runtime.hicache.write_completion") event.set_arg("blocking", "true");
        blocking.set_runtime_observations(std::move(observations));
        hc::runtime::HiCacheQueueConfirmations unsupported;
        hicache_timing_fixture::require_throws<std::runtime_error>([&] { unsupported.bind(blocking, replay, 0, 121); },
                                                                   "a blocking write return cannot provide an ordinary nonblocking ACK boundary");
    }
    for (const std::string kind : {"write", "storage", "load"}) {
        const bool storage = kind == "storage";
        const bool load = kind == "load";
        auto clipped = graph;
        auto observations = clipped.runtime_observations();
        if (storage) {
            for (auto & event : observations) {
                if (event.name == "runtime.cpu_collective" && event.ts == 85) {
                    event.set_arg("role", "storage_control_drain");
                    event.set_arg("numel", "3");
                }
                if (event.name == "runtime.hicache.write_completion" && event.ts == 84) {
                    event.name = "runtime.hicache.storage_drain";
                    event.ts = 101; event.dur = 4;
                    for (const auto* field : {"n_revoke", "n_backup", "n_release"}) event.set_arg(field, "0");
                    event.set_arg("tp_world_size", "2");
                }
            }
        }
        if (load) {
            for (auto & event : observations) {
                if (event.name == "runtime.cpu_collective" && event.ts == 85)
                    event.set_arg("role", "load_completion_check");
                if (event.name == "runtime.hicache.write_completion" && event.ts == 84)
                    event.name = "runtime.hicache.load_completion";
            }
        }
        clipped.set_runtime_observations(observations);
        hc::runtime::HiCacheQueueConfirmations boundary;
        boundary.bind(clipped, replay, 0, 100);
        require(boundary.coverage().partial_window_rounds == 0
                && boundary.coverage().write_rounds == (storage || load ? 2 : 3)
                && boundary.coverage().load_rounds == (load ? 1 : 0)
                && boundary.coverage().storage_rounds == (storage ? 1 : 0),
                "complete MIN retains its observed application tail beyond the HTTP endpoint");
        auto no_boundary = graph;
        // A return inside a retained CPU leaf is not a gap boundary. A return
        // beyond the last leaf, in contrast, legitimately proves a tail gap.
        for (size_t id = 0; id < no_boundary.node_count(); ++id) {
            auto & event = no_boundary.mutable_event_for_node(id);
            if (event.pid == "2" && event.name == "after") {
                event.dur = 20;
                no_boundary.mutable_node(id).duration = 20;
            }
        }
        auto unbound_observations = observations;
        for (auto & event : unbound_observations)
            if (event.pid == "2" && event.ts >= 84
                && event.name == (storage ? "runtime.hicache.storage_drain"
                    : load ? "runtime.hicache.load_completion" : "runtime.hicache.write_completion"))
                event.dur = 130 - event.ts;
        no_boundary.set_runtime_observations(std::move(unbound_observations));
        hc::runtime::HiCacheQueueConfirmations unbound;
        hicache_timing_fixture::require_throws<std::runtime_error>([&] { unbound.bind(no_boundary, replay, 0, 100); },
                                                                   "a returned envelope without its retained CPU boundary cannot execute");
        // A missing rank return must still fail, even if another rank proves
        // that its application crossed the window.
        std::erase_if(observations, [&](const auto & event) {
            return event.pid == "2" && event.name == (storage ? "runtime.hicache.storage_drain"
                : load ? "runtime.hicache.load_completion" : "runtime.hicache.write_completion")
                   && event.ts >= 84;
        });
        auto missing = graph;
        missing.set_runtime_observations(std::move(observations));
        hc::runtime::HiCacheQueueConfirmations incomplete;
        hicache_timing_fixture::require_throws<std::runtime_error>([&] { incomplete.bind(missing, replay, 0, 100); },
                                                                   "partial application cannot excuse another rank's missing return observation");
    }
    {
        // A zero-operation calibration is sufficient to prepare the source
        // tails; no target timings enter this boundary test.
        using Json = nlohmann::json;
        const Json node{{"name", "empty ACK"}, {"category", "test"}, {"is_cpu", true},
                        {"resource", "main"}, {"duration", 1}, {"cpu_gap_after", 0}, {"counts_toward_e2e", true}};
        const Json host{{"nodes", Json::array({node})}, {"edges", Json::array()}, {"streams", Json::array()},
                        {"waits", Json::array()}, {"event_waits", Json::array()}, {"host_return", 0}};
        Json rows = Json::array();
        std::set<int> ranks;
        for (size_t id = 0; id < graph.node_count(); ++id) ranks.insert(graph.node(id).gpu_id);
        for (const auto rank : ranks)
            rows.push_back({{"status", "ready"}, {"rank", rank}, {"confirmed_operations", 0},
                            {"confirmed_batches", 0}, {"main_cpu_us", 1}, {"main_gap_us", 0}, {"template", host}});
        const auto path = std::filesystem::temp_directory_path() / ("markov_ack_head_" + std::to_string(getpid()) + ".json");
        std::ofstream(path) << Json{{"role", "fixed_calibration"}, {"source_manifest", "test-fixture"}, {"rows", rows}};
        std::vector<uint64_t> ends;
        for (const std::optional<uint64_t> idle : {std::optional<uint64_t>{}, std::optional<uint64_t>{0}}) {
            auto head = graph;
            hc::runtime::HiCacheWriteConfirmations costs;
            costs.prepare(head, path.string(), 25, 121, {}, idle);
            costs.replace_source_tails(head);
            (void)simulation::run_topological_simulation(head);
            uint64_t end = 0;
            for (size_t id = 0; id < head.node_count(); ++id)
                if (head.event_for_node(id).name == "after") end = std::max(end, head.node(id).completion_time);
            ends.push_back(end);
        }
        std::filesystem::remove(path);
        require(ends[0] == ends[1] + 6, "idle admission removes only the six-us post-MIN head tail on each parallel rank");
    }
    // The first wrapper starts at 24, before the HTTP cut at 25. Only
    // an earlier idle barrier can supply its missing queue context.
    for (const std::optional<uint64_t> idle : {std::optional<uint64_t>{}, std::optional<uint64_t>{0}, std::optional<uint64_t>{25}}) {
        auto head = graph;
        hc::model::HiCacheModelReplay empty(head, config, true);
        hc::runtime::HiCacheQueueConfirmations boundary;
        size_t returned = 0;
        boundary.bind(head, empty, 25, 121, {}, [&](const auto & result) {
            require(result.confirmed_writes == 0, "idle head must sample the target's empty queue");
            ++returned;
        }, {}, idle);
        const bool admitted = idle && *idle <= 24;
        require(boundary.coverage().partial_window_rounds == (admitted ? 0 : 1),
                "head admission requires an idle barrier before the wrapper");
        (void)simulation::run_topological_simulation(head, {}, [&](size_t node, uint64_t time, simulation::FutureDag & future) {
            boundary.advance(node, time, future);
        });
        require(returned == (admitted ? 6 : 4), "admitted head retains both rank returns");
    }
    {
        auto head = graph;
        hc::model::HiCacheModelReplay occupied(head, config, true);
        auto fact = hicache_timing_fixture::request_fact("cache_lifecycle_commit", "occupied", 0, 0);
        fact.pid = fact.tid = "1";
        fact.cache_scope = "cache:1";
        fact.lifecycle_kind = "finished";
        occupied.state().apply_fact(fact, hc::HiCacheFactRole::CacheLifecycleCommit);
        hc::runtime::HiCacheQueueConfirmations boundary;
        boundary.bind(head, occupied, 25, 121, {}, {}, {}, 0);
        bool rejected = false;
        try {
            (void)simulation::run_topological_simulation(head, {}, [&](size_t node, uint64_t time, simulation::FutureDag & future) {
                fact.ts = time;
                hicache_timing_fixture::complete_test_writes(occupied.state(), fact);
                boundary.advance(node, time, future);
            });
        } catch (const std::runtime_error & error) {
            rejected = std::string(error.what()).find("idle target prefix") != std::string::npos;
        }
        require(rejected, "idle evidence cannot override a nonempty live target prefix");
    }
    std::vector<hc::runtime::QueueConfirmationReturn> returns;
    hc::runtime::HiCacheQueueConfirmations confirmations;
    size_t work_calls = 0;
    hc::runtime::HiCacheQueueConfirmations::WriteWork work;
    if (with_work) work = [&](const hc::runtime::WriteConfirmationWork & item, simulation::FutureDag & future) -> std::optional<size_t> {
        ++work_calls;
        require(item.confirmed_writes == (item.round == 0 ? 0 : 1), "work must use the sampled common prefix, not later DMA completions");
        if (!item.confirmed_writes) return std::nullopt;
        require(replay.state().unacknowledged_device_writes(item.fact).size() == (item.round == 1 ? 2 : 1),
                "confirmation work must begin before state publication");
        const auto tail = future.append({.name = "target ACK work", .category = "test", .is_cpu = true,
                                         .lane_key = "ack:" + item.fact.pid, .duration = 9});
        future.depend(*item.fact.execution_anchor_node_id, tail);
        return tail;
    };
    confirmations.bind(graph, replay, 0, 100, {}, [&](const auto & result) {
        returns.push_back(result);
        auto fact = hicache_timing_fixture::request_fact("cache_lifecycle_commit", "inspect", result.timestamp_us, 0);
        fact.pid = fact.tid = std::to_string(result.rank + 1);
        fact.cache_scope = "cache:" + fact.pid;
        require(replay.state().prefetch_queue_sizes(fact).backup_acks == (result.round == 2 ? 1 : 0),
                "H2S must start at write ACK return, not at DMA completion or MIN sampling");
    }, work);
    require(confirmations.coverage().write_rounds == 3 && confirmations.coverage().load_rounds == 0,
            "ordinary write rounds must be observed independently of load rounds");
    (void)simulation::run_topological_simulation(graph, {}, [&](size_t node, uint64_t time, simulation::FutureDag & future) {
        if (graph.event_for_node(node).name == "second_write") backup(graph.event_for_node(node).pid, time, 1'000);
        for (int rank = 0; rank < 2; ++rank) {
            auto fact = hicache_timing_fixture::request_fact("cache_lifecycle_commit", "dma", time, 0);
            fact.cache_scope = "cache:" + std::to_string(rank + 1);
            hicache_timing_fixture::complete_test_writes(replay.state(), fact);
        }
        confirmations.advance(node, time, future);
    });
    require(returns.size() == 6, "every rank must apply each sampled write MIN");
    require(work_calls == (with_work ? 6 : 0), "optional ACK work is emitted once per rank agreement");
    for (const auto & result : returns) {
        require(result.write && !result.storage && result.confirmed_loads == 0, "write ACK is not a load or storage drain");
        require(result.timestamp_us == 45 + result.round * (with_work ? 33 : 30),
                "ACK publication must wait for target work, preserving the original return when work is disabled");
        require(result.confirmed_writes == (result.round == 0 ? 0 : 1), "late completion cannot change the sampled prefix");
        require(result.remaining_writes == (result.round == 2 ? 0 : 1), "unsampled completion must wait for the next round");
        if (result.round == 0) require(result.local_writes == (result.rank == 0 ? 1 : 0), "rank readiness must remain asymmetric before MIN");
    }
    for (int rank = 0; rank < 2; ++rank) {
        auto fact = hicache_timing_fixture::request_fact("cache_lifecycle_commit", "inspect", 135, 0);
        fact.pid = fact.tid = std::to_string(rank + 1);
        fact.cache_scope = "cache:" + fact.pid;
        require(replay.state().prefetch_queue_sizes(fact).backup_acks == 2, "second H2S finishes 20us after its later write ACK");
    }
}
} // namespace

void check_prefetch_worker_observations() {
    ordinary_write_confirmations();
    ordinary_write_confirmations(true);
    for (const auto * role : { "load_completion_check", "write_completion_check" }) {
        // Removed wait regions may no longer have native communication nodes.
        // Their metadata must be excluded before binding global confirmations.
        auto calls = fixture();
        std::erase_if(calls, [](const auto & event) {
            return event.name != "runtime.cpu_collective" || event.arg("role") != "prefetch_storage_hit_agreement";
        });
        for (auto & call : calls) call.set_arg("role", role);
        core::DagGraph graph;
        graph.set_runtime_observations(calls);
        hc::model::HiCacheModelReplay replay(graph, hicache_timing_fixture::timing_config(), true);
        hc::runtime::HiCacheQueueConfirmations replaced;
        replaced.bind(graph, replay, 0, 100, calls);
        require(replaced.coverage().replaced_rounds == 1 && replaced.coverage().load_rounds == 0 && replaced.coverage().write_rounds == 0 && graph.node_count() == 0,
                "a replaced whole-rank confirmation must not bind or apply twice");
        hc::runtime::HiCacheQueueConfirmations partial;
        bool rejected = false;
        try { partial.bind(graph, replay, 0, 100, {calls.front()}); }
        catch (const std::runtime_error &) { rejected = true; }
        require(rejected, "replacing only one rank cannot suppress a shared confirmation");
        auto envelopes = calls;
        uint64_t begin = calls.front().ts;
        for (const auto & call : calls) {
            begin = std::min(begin, call.ts);
            auto envelope = call;
            envelope.name = std::string(role) == "load_completion_check"
                ? "runtime.hicache.load_completion" : "runtime.hicache.write_completion";
            envelope.ts -= 2;
            envelope.dur += 4;
            envelope.set_arg("status", "returned");
            envelope.set_arg("blocking", "false");
            envelopes.push_back(std::move(envelope));
        }
        graph.set_runtime_observations(envelopes);
        hc::runtime::HiCacheQueueConfirmations prestarted;
        prestarted.bind(graph, replay, begin, 100);
        require(prestarted.coverage().partial_window_rounds == 1 && graph.node_count() == 0,
                "a complete MIN inside a prestarted call must not rebuild its missing CPU prefix");
        hc::runtime::HiCacheQueueConfirmations whole_call;
        rejected = false;
        try { whole_call.bind(graph, replay, 0, 100); }
        catch (const std::runtime_error & error) {
            rejected = std::string(error.what()).find("incomplete_cpu_interval") != std::string::npos;
        }
        require(rejected, "a fully included operation still requires complete CPU evidence");
        envelopes.pop_back();
        graph.set_runtime_observations(envelopes);
        hc::runtime::HiCacheQueueConfirmations absent_return;
        rejected = false;
        try { absent_return.bind(graph, replay, begin, 100); }
        catch (const std::runtime_error &) { rejected = true; }
        require(rejected, "a prestarted call still requires both ranks' return envelopes");
        calls.back().ts += 5;
        graph.set_runtime_observations(calls);
        hc::runtime::HiCacheQueueConfirmations clipped;
        clipped.bind(graph, replay, 0, 25);
        require(clipped.coverage().partial_window_rounds == 1 && clipped.coverage().load_rounds == 0 && graph.node_count() == 0,
                "a round crossing the selected window must report incomplete coverage, not invent a rank or confirmation");
        hc::runtime::HiCacheQueueConfirmations missing;
        rejected = false;
        try { missing.bind(graph, replay, 0, 100); }
        catch (const std::runtime_error &) { rejected = true; }
        require(rejected, "unreplaced confirmations require real communication and return boundaries");
    }
    {
        auto events = fixture();
        auto second = fixture();
        for (auto & event : second) {
            event.ts += 120;
            if (!event.arg("request_id").empty()) event.set_arg("request_id", "second");
            if (!event.arg("operation_id").empty()) event.set_arg("operation_id", "84");
            if (event.name == "runtime.cpu_collective") {
                event.set_arg("collective_index", "1");
                event.set_arg("sequence_before", "1");
                event.set_arg("sequence_after", "2");
            }
            event.index = events.size();
            events.push_back(std::move(event));
        }
        const auto graph = core::DagBuilder(1).build(std::move(events), 0);
        const hc::patch::HiCacheSourceDagIndex source(graph);
        const auto rounds = hc::observe_cpu_collectives(source);
        const auto workers = hc::observe_prefetch_workers(source, rounds);
        require(workers.issues.empty(), "two complete query episodes must remain independently attributable");
        const auto first = hc::runtime::observe_prefetch_query_template(graph, rounds, workers, 0, 300);
        require(first.issue.empty() && first.observations.size() == 2, "query template comes from existing graph observations");
        for (const auto & [rank, id] : first.observations) {
            require(graph.runtime_observations()[id].arg("request_id") == "r", "first complete query chosen without target evidence");
            const auto & cpu = first.timing.cpu.at(rank);
            require(cpu.before_sample == 12 && cpu.sample_to_min == 2 && cpu.min_to_apply == 2,
                    "query CPU includes measured dispatch and keeps sampling/MIN/return costs separate");
            require(first.timing.agreement.calls.at(rank).envelope_remainder_us == 2, "query retains measured collective envelope");
        }
        const auto later = hc::runtime::observe_prefetch_query_template(graph, rounds, workers, 100, 300);
        require(later.issue.empty() && graph.runtime_observations()[later.observations.at(0)].arg("request_id") == "second",
                "prelude query cannot replace a formal-window template");
        require(!hc::runtime::observe_prefetch_query_template(graph, rounds, workers, 0, 25).issue.empty(),
                "query whose return is outside the window is not a complete donor");
        auto incomplete = workers;
        incomplete.queries.front().issue = "missing_evidence";
        const auto fallback = hc::runtime::observe_prefetch_query_template(graph, rounds, incomplete, 0, 300);
        require(fallback.issue.empty() && graph.runtime_observations()[fallback.observations.at(0)].arg("request_id") == "second",
                "one rank of an incomplete episode cannot supply a complete group template");
    }
    {
        std::vector<core::TraceEvent> events;
        for (const auto * name : { "runtime.hicache.load_completion", "runtime.hicache.write_completion", "runtime.hicache.decode_allocation",
                                  "runtime.hicache.capacity_guard", "runtime.hicache.device_release_backup", "runtime.hicache.device_release_regular",
                                  "runtime.hicache.allocator_free",
                                  "runtime.hicache.load_allocation" }) {
            core::TraceEvent e;
            e.name = name;
            e.pid = e.tid = "1";
            e.ts = 10;
            e.dur = 20;
            e.source_channel = core::TraceSourceChannel::PythonProbe;
            e.cat = "runtime_diagnostic";
            events.push_back(std::move(e));
        }
        const auto graph = core::DagBuilder(1).build(std::move(events), 0);
        require(graph.runtime_observations().size() == 8 && graph.nodes().empty(),
                "runtime envelopes must remain observations: observations=" + std::to_string(graph.runtime_observations().size())
                + " executable_nodes=" + std::to_string(graph.nodes().size()));
    }
    const auto good = inspect(fixture());
    require(good.issues.empty(), "two-rank query/MIN/release and drain must bind without inventing Torch worker costs");
    require(good.queries.size() == 2 && good.releases.size() == 6 && good.drains.size() == 2, "all low-frequency source observations are retained");
    for (const auto & q : good.queries)
        require(q.candidate_fact && q.enqueue && q.release && q.agreement_rounds.size() == 1,
                "query binds its candidate even when it starts before candidate and submit return");
    for (const auto & r : good.releases)
        require(r.pages && (r.owner == "query" || r.owner == "worker" || r.owner == "prefetch_completion"), "release stage and zero pages stay explicit");
    require(inspect(fixture(true)).issues.empty(), "single-rank source needs no invented collective");
    auto events = fixture();
    std::erase_if(events, [](const auto & e) { return e.name == "runtime.hicache.prefetch_enqueue"; });
    check_issue(inspect(events), "missing_or_nonunique_enqueue");
    events = fixture();
    std::erase_if(events, [](const auto & e) { return e.name == "hicache_candidate"; });
    check_issue(inspect(events), "missing_or_nonunique_enqueue_candidate");
    events = fixture();
    auto extra_candidate = *std::ranges::find_if(events, [](const auto & e) { return e.name == "hicache_candidate"; });
    extra_candidate.index = events.size();
    events.push_back(extra_candidate);
    check_issue(inspect(events), "missing_or_nonunique_enqueue_candidate");
    events = fixture();
    std::erase_if(events, [](const auto & e) { return e.name == "runtime.cpu_collective" && e.arg("role") == "prefetch_storage_hit_agreement"; });
    check_issue(inspect(events), "query_min_count_mismatch");
    events = fixture();
    for (auto & e : events)
        if (e.name == "runtime.hicache.host_release" && e.tid == "worker") e.set_arg("token_count", "0");
    check_issue(inspect(events), "worker_release_page_count_mismatch");
    events = fixture();
    auto duplicate = *std::ranges::find_if(events, [](const auto & e) { return e.name == "runtime.hicache.host_release" && e.tid == "worker"; });
    duplicate.ts += 2;
    events.push_back(duplicate);
    check_issue(inspect(events), "multiple_worker_releases");
    events = fixture();
    for (auto & e : events)
        if (e.name == "runtime.hicache.prefetch_query" && e.pid == "2") e.set_arg("request_id", "different request");
    check_issue(inspect(events), "query_min_request_mismatch");
    events = fixture();
    for (auto & e : events)
        if (e.name == "runtime.hicache.host_release" && e.tid == "query") e.ts = 21;
    check_issue(inspect(events), "query_min_count_mismatch");
    events = fixture();
    for (auto & e : events)
        if (e.name == "runtime.hicache.storage_drain") e.set_arg("status", "raised");
    check_issue(inspect(events), "invalid_drain_result");
    events = fixture(true);
    for (auto & e : events)
        if (e.name == "runtime.hicache.storage_drain") {
            e.set_arg("n_revoke", "null");
            e.set_arg("n_backup", "null");
            e.set_arg("n_release", "null");
        }
    const auto local = inspect(events);
    require(local.issues.empty() && local.drains[0].local_shutdown && !local.drains[0].released_pages, "unbounded shutdown drain is not zero released pages");
    events = fixture(true);
    for (auto & e : events)
        if (e.name == "runtime.hicache.host_release" && e.tid == "main") e.set_arg("role", "prefetch_abort");
    const auto aborted = inspect(events);
    require(aborted.issues.empty() && std::ranges::any_of(aborted.releases, [](const auto & r) { return r.owner == "prefetch_abort" && !r.parent_fact; }),
            "abort release must not be disguised as a background or successful progress return");
}
