#include "hicache_timing_fixture.hpp"
#include "markov/trace_graph/core/cpu_gap_observation.hpp"
#include "markov/trace_graph/core/dag_builder.hpp"
#include "markov/trace_graph/modules/hicache/cpu_collective_timing.hpp"
#include "markov/trace_graph/modules/hicache/dag_patch_module.hpp"
#include "markov/trace_graph/modules/hicache/patch/cpu_collective_waits.hpp"
#include "markov/trace_graph/simulation/topological_simulator.hpp"
#include <nlohmann/json.hpp>
#include <stdexcept>

using namespace markov::trace_graph;
namespace hc = modules::hicache;
namespace {
void require(bool value, const char * message) {
    if (!value) throw std::runtime_error(message);
}
core::DagGraph fixture(bool self, bool twice) {
    std::vector<core::TraceEvent> events;
    for (int rank = 0; rank < 2; ++rank) {
        const auto pid = std::to_string(rank + 1);
        const uint64_t shift = rank * 30;
        auto add = [&](const char * name, const std::string & tid, uint64_t ts, uint64_t dur) -> core::TraceEvent & {
            core::TraceEvent event;
            event.name = name;
            event.pid = pid;
            event.tid = tid;
            event.ts = ts;
            event.dur = dur;
            event.cat = "cpu_op";
            event.source_channel = core::TraceSourceChannel::Torch;
            events.push_back(std::move(event));
            return events.back();
        };
        add("producer", pid, 0, 1 + shift);
        for (int index = 0; index < (twice ? 2 : 1); ++index) {
            const uint64_t offset = index * 90;
            add("c10d::allreduce_", pid, 10 + shift + offset, 20);
            add("gloo:all_reduce", "worker", 12 + shift + offset, rank ? 28 : 38);
            if (self) add("hicache.control.prefetch_progress", pid, 5 + shift + offset, rank ? 45 : 55);
            auto & call = add("runtime.cpu_collective", pid, 5 + shift + offset, rank ? 45 : 55);
            call.cat = "runtime_diagnostic";
            call.source_channel = core::TraceSourceChannel::PythonProbe;
            hicache_timing_fixture::set_collective_args(call, rank, index);
        }
        add("consumer", pid, 90 + (rank ? 20 : 0) + (twice ? 90 : 0), 1);
    }
    return core::DagBuilder(1).build(std::move(events), 0);
}
hc::patch::CpuCollectiveWaitPlan plan(const core::DagGraph & graph) {
    const hc::patch::HiCacheSourceDagIndex source(graph);
    const auto observations = hc::observe_cpu_collectives(source);
    require(observations.issues.empty(), "fixture has full collective observations");
    std::vector<const hc::CpuCollectiveRound *> rounds;
    for (const auto & round : observations.rounds) { rounds.push_back(&round); }
    return hc::patch::plan_cpu_collective_waits(source, rounds);
}
size_t find(const core::DagGraph & graph, const char * name, const char * pid) {
    for (const auto & node : graph.nodes())
        if (graph.event_for_node(node.id).name == name && graph.event_for_node(node.id).pid == pid) return node.id;
    throw std::runtime_error("missing fixture node");
}
core::DagGraph broadcast_fixture(const std::vector<int> & members, size_t root, bool group_root) {
    std::vector<core::TraceEvent> events;
    for (const int rank : members) {
        const auto pid = std::to_string(rank + 100);
        auto add = [&](const char * name, const std::string & tid, uint64_t ts, uint64_t dur) -> core::TraceEvent & {
            core::TraceEvent event;
            event.name = name;
            event.pid = pid;
            event.tid = tid;
            event.ts = ts;
            event.dur = dur;
            event.cat = "cpu_op";
            event.source_channel = core::TraceSourceChannel::Torch;
            events.push_back(std::move(event));
            return events.back();
        };
        add("producer", pid, 0, 1);
        add("c10d::broadcast_", pid, 10, 5);
        add("gloo:broadcast", "worker", 12, 10);
        add("consumer", pid, 30, 1);
        auto & call = add("runtime.cpu_collective", pid, 5, 20);
        call.cat = "runtime_diagnostic";
        call.source_channel = core::TraceSourceChannel::PythonProbe;
        call.set_arg("group", "4");
        call.set_arg("members", nlohmann::json(members).dump());
        call.set_arg("rank", std::to_string(rank));
        call.set_arg("collective_index", "0");
        call.set_arg("operation", "broadcast");
        call.set_arg(group_root ? "group_src" : "src", std::to_string(group_root ? root : members[root]));
        call.set_arg("numel", "1");
        call.set_arg("dtype", "torch.int64");
        call.set_arg("sequence_before", "0");
        call.set_arg("sequence_after", "1");
        call.set_arg("async_op", "false");
        call.set_arg("status", "returned");
    }
    return core::DagBuilder(1).build(std::move(events), 0);
}
void check_broadcast_branches() {
    for (const auto members : {
             std::vector<int>{ 0, 1, 2, 3 },
             std::vector<int>{ 7, 3, 11 },
             std::vector<int>{ 9, 2, 8, 4, 13 }
    })
        for (size_t root = 0; root < members.size(); ++root)
            for (const bool group_root : { false, true }) {
                auto graph = broadcast_fixture(members, root, group_root);
                const auto rewrite = plan(graph);
                require(rewrite.issues.empty() && rewrite.calls == members.size(), "all broadcast roots and memberships are supported");
                require(core::validate_dag_mutation_plan(graph, rewrite.mutation).ok(), "broadcast plan is valid");
                (void)core::apply_dag_mutation_plan(graph, rewrite.mutation);
                require(simulation::run_topological_simulation(graph).e2e_us == 31, "broadcast same-cost complete time is unchanged");
                const auto node = [&](const char * name, size_t virtual_rank) {
                    return find(graph, name, std::to_string(members[(virtual_rank + root) % members.size()] + 100).c_str());
                };
                for (size_t delayed = 0; delayed < members.size(); ++delayed) {
                    graph.set_node_duration(node("producer", delayed), 101);
                    (void)simulation::run_topological_simulation(graph);
                    require(graph.node(node("consumer", delayed)).completion_time == 131, "late local entry is retained");
                    if (delayed == 0)
                        for (size_t rank = 0; rank < members.size(); ++rank)
                            require(graph.node(node("consumer", rank)).completion_time == 131, "late root affects the whole tree");
                    if (delayed == 2) {
                        require(graph.node(node("consumer", 0)).completion_time == 131, "root waits for its direct receiver");
                        require(graph.node(node("consumer", 1)).completion_time == 31, "late sibling does not block the other branch");
                    }
                    if (delayed == 3) {
                        require(graph.node(node("consumer", 1)).completion_time == 131, "intermediate sender waits for its child");
                        require(graph.node(node("consumer", 0)).completion_time == 31, "root does not wait for a grandchild");
                        require(graph.node(node("consumer", 2)).completion_time == 31, "other branch does not wait for a grandchild");
                    }
                    graph.set_node_duration(node("producer", delayed), 1);
                }
            }
}
void check_cross_scoring_labels_do_not_block_communication() {
    for (const bool changed : { false, true }) {
        for (const bool self : { false, true }) {
            auto graph = fixture(false, false);
            const auto owner = find(graph, "producer", "1");
            if (changed) graph.set_cpu_gap_after(owner, graph.node(owner).cpu_gap_after + 1);
            const auto baseline = simulation::run_topological_simulation(graph).e2e_us;
            graph.add_scope_gap_duration(owner, 1);
            graph.set_input_contracts({ "hicache_dag_patch" });
            // A source/target-absent visibility opportunity makes a complete no-op
            // HiCache transaction, so the actual module can compose communication.
            core::TraceEvent fact;
            fact.index = 900;
            fact.name = "hicache_prefetch_candidate";
            fact.source_channel = core::TraceSourceChannel::PythonProbe;
            fact.pid = fact.tid = "1";
            fact.ts = 1;
            fact.set_arg("fact", R"({"class":"workload_identity","role":"prefetch_candidate_anchor","consumers":["hicache_dag_patch"]})");
            fact.set_arg("phase", "end");
            fact.set_arg("request_id", "request");
            graph.set_hicache_fact_events({ fact });
            auto result = std::make_shared<hc::model::HiCacheModelResult>();
            result->replay_complete = true;
            result->effect_decisions.status = "ready";
            hc::model::HiCacheEffectDecision decision;
            decision.effect_key = decision.effect_family_key = "visibility";
            decision.effect_type = hc::model::HiCacheEffectType::PrefetchVisibility;
            decision.target_effect_state = hc::model::HiCacheTargetEffectState::NotRequired;
            decision.source_node_id = 900;
            decision.source_fact_role = "prefetch_candidate_anchor";
            result->effect_decisions.decisions.push_back(decision);
            hc::HiCacheDagPatchModule module(result, self);
            module.apply(graph);
            if (!module.result().apply_blockers.empty())
                throw std::runtime_error("scoring-label fixture blocked: " + nlohmann::json(module.result().apply_blockers).dump());
            require(simulation::run_topological_simulation(graph).e2e_us == baseline, "scoring labels cannot change same-cost full replay");
            if (self) { require(graph.scope_gap_duration(owner) == 1, "self keeps source component annotations"); }
            else {
                require(module.result().cpu_collective_patch.planned_rounds == (changed ? 0 : 1), "cross rejects changed costs, not component labels alone");
                if (!changed) {
                    graph.set_node_duration(find(graph, "producer", "2"), 1);
                    require(simulation::run_topological_simulation(graph).e2e_us == 81, "cross communication responds to earlier peer availability");
                }
            }
        }
    }
}

void check_composition() {
    for (const bool twice : { false, true }) {
        auto graph = fixture(true, twice);
        const auto baseline = simulation::run_topological_simulation(graph).e2e_us;
        const hc::patch::HiCacheSourceDagIndex source(graph);
        const auto observed = hc::observe_cpu_collectives(source);
        core::DagMutationPlan combined{ .component = "hicache" };
        combined.synthetic_nodes.push_back({
            .synthetic_id = "independent",
            .node = { .name = "independent", .duration = 1 },
            .effect_id = "other"
        });
        auto audit = hc::patch::append_retained_cpu_collectives(source, observed, combined);
        require(audit.status == "ready" && audit.planned_rounds == (twice ? 2 : 1), "independent rounds join the complete transaction");
        require(core::validate_dag_mutation_plan(graph, combined).ok(), "shared gaps are split once across all selected calls");
        (void)core::apply_dag_mutation_plan(graph, combined);
        require(simulation::run_topological_simulation(graph).e2e_us == baseline, "composition retains ordinary timing and independent work");
        require(simulation::run_gap_excluded_topological_simulation(graph).e2e_us == 0,
                "unowned communication gaps do not silently enter the HiCache-only scope");
        for (const auto & node : graph.nodes())
            if (node.kind == core::DagNodeKind::Synthetic && node.cpu_gap_after) {
                const auto owned = std::min<uint64_t>(3, node.cpu_gap_after);
                graph.add_scope_gap_duration(node.id, owned);
                require(simulation::run_gap_excluded_topological_simulation(graph).e2e_us == owned,
                        "synthetic gaps can enter a scope only through explicit ownership");
                break;
            }
    }
    const auto graph = fixture(true, false);
    const hc::patch::HiCacheSourceDagIndex source(graph);
    const auto observed = hc::observe_cpu_collectives(source);
    for (const auto * name : { "c10d::allreduce_", "gloo:all_reduce" }) {
        core::DagMutationPlan combined{ .component = "hicache" };
        const auto id = find(graph, name, "1");
        combined.set_node_durations.push_back({ .node_id = id, .duration = 0, .effect_id = "prefetch" });
        const auto audit = hc::patch::append_retained_cpu_collectives(source, observed, combined);
        require(audit.planned_rounds == 0 && audit.source_retained_rounds.at("shared_transaction_boundary") == 1,
                "changing one participant preserves the whole round for joint planning");
        require(combined.set_node_durations.size() == 1 && combined.synthetic_nodes.empty(), "an intersecting round does not overwrite prior ownership");
    }
    core::DagMutationPlan combined{ .component = "hicache" };
    combined.add_edges.push_back({ .src = core::DagNodeRef::existing(find(graph, "producer", "1")),
                                   .dst = core::DagNodeRef::existing(find(graph, "c10d::allreduce_", "1")),
                                   .kind = core::DagEdgeKind::Mutation,
                                   .effect_id = "prefetch" });
    const auto audit = hc::patch::append_retained_cpu_collectives(source, observed, combined);
    require(audit.planned_rounds == 0 && combined.add_edges.size() == 1, "new submission dependencies cannot be bypassed by a separately planned worker fork");
    auto incomplete = observed;
    incomplete.rounds.front().issue = "collective_membership_mismatch";
    combined = { .component = "hicache" };
    const auto missing = hc::patch::append_retained_cpu_collectives(source, incomplete, combined);
    require(missing.source_retained_rounds.at("incomplete_observation") == 1 && combined.empty(), "missing coverage remains explicit source cost");
}
} // namespace

void check_cpu_collective_waits() {
    {
        auto graph = fixture(false, true);
        const auto change = plan(graph);
        require(change.issues.empty(), "boundary fixture needs complete communication timing");
        (void)core::apply_dag_mutation_plan(graph, change.mutation);
        (void)simulation::run_topological_simulation(graph);
        const auto baseline = graph.nodes();
        const hc::patch::HiCacheSourceDagIndex source(graph);
        std::vector<core::TraceEvent> points;
        for (int rank = 0; rank < 2; ++rank)
            for (const uint64_t at : { 5UL + rank * 30, 60UL + rank * 20, 95UL + rank * 30, 150UL + rank * 20 }) {
                core::TraceEvent point;
                point.name = "confirmation";
                point.pid = point.tid = std::to_string(rank + 1);
                point.ts = at;
                points.push_back(std::move(point));
            }
        auto nodes = core::insert_cpu_gap_points(graph, points);
        for (size_t i = 0; i < points.size(); ++i) {
            if (!nodes[i]) nodes[i] = source.cpu_node_starting_at(points[i].pid, points[i].tid, points[i].ts);
            require(nodes[i].has_value(), "reconstructed communication must retain exact sample/return observation sites");
        }
        (void)simulation::run_topological_simulation(graph);
        for (const auto & node : baseline)
            require(graph.node(node.id).simulation_start == node.simulation_start && graph.node(node.id).completion_time == node.completion_time,
                    "binding later observations must not change any existing communication timing");
    }
    check_cross_scoring_labels_do_not_block_communication();
    check_composition();
    check_broadcast_branches();
    for (const bool self : { false, true })
        for (const bool twice : { false, true }) {
            auto graph = fixture(self, twice);
            const auto baseline = simulation::run_topological_simulation(graph).e2e_us;
            const auto observed = graph;
            auto rewrite = plan(graph);
            if (!rewrite.issues.empty()) throw std::runtime_error("collective plan issues: " + nlohmann::json(rewrite.issues).dump());
            require(rewrite.calls == (twice ? 4 : 2), "all requested calls can be planned");
            require(core::validate_dag_mutation_plan(graph, rewrite.mutation).ok(), "fork and join mutation is valid");
            const auto mutation = core::apply_dag_mutation_plan(graph, rewrite.mutation);
            require(simulation::run_topological_simulation(graph).e2e_us == baseline, "same-cost complete replay is unchanged");
            for (const auto & node : observed.nodes()) {
                if (observed.event_for_node(node.id).name.ends_with(".self")) continue;
                const auto & event = observed.event_for_node(node.id);
                if (event.name == "gloo:all_reduce") {
                    // Disconnected worker roots used to replay from zero.
                    require(graph.node(node.id).completion_time == event.ts + event.dur, "bound worker finishes at its observed source time");
                    continue;
                }
                if (graph.node(node.id).completion_time != node.completion_time)
                    throw std::runtime_error("same-cost node " + observed.event_for_node(node.id).name + " pid=" + observed.event_for_node(node.id).pid
                                             + " changed " + std::to_string(node.completion_time) + " -> "
                                             + std::to_string(graph.node(node.id).completion_time));
            }
            for (const auto & [key, id] : mutation.synthetic_node_ids) {
                if (key.ends_with(":submit_start") || key.ends_with(":return"))
                    require(graph.event_for_node(id).ts > 0, "observed main-thread boundaries retain their source positions");
                else require(graph.event_for_node(id).ts == 0, "internal timing nodes do not fabricate source timestamps");
                require(graph.node(id).duration == 0, "dispatch and return residuals remain gaps rather than CPU work");
            }
            graph.set_node_duration(find(graph, "producer", "2"), 1);
            const auto faster = simulation::run_topological_simulation(graph).e2e_us;
            require(faster < baseline, "an earlier peer releases relevant waiting");
            require(graph.node(find(graph, "c10d::allreduce_", "1")).duration == 20, "submission work is not discarded with waiting");
            if (!twice) {
                require(faster == 81 && graph.node(find(graph, "consumer", "1")).completion_time == 71,
                        "return joins submission and worker and retains all outside gaps");
                const hc::patch::HiCacheSourceDagIndex source(observed);
                const auto calls = hc::observe_cpu_collectives(source);
                for (const auto & call : calls.rounds.front().calls) {
                    const auto id = mutation.synthetic_node_ids.at("cpu_collective:" + std::to_string(call.observation) + ":return");
                    // With both entries at 5 us, rank 0 joins submission at
                    // 30 us; rank 1 joins its worker at 40 us. Return adds 10 us.
                    require(graph.node(id).completion_time == (call.rank == 0 ? 40 : 50),
                            "changed peer arrival preserves submission/worker overlap and return cost");
                }
            }
        }
    auto graph = fixture(false, false);
    graph.mutable_node(find(graph, "c10d::allreduce_", "1")).cpu_gap_after = 0;
    auto rejected = plan(graph);
    require(!rejected.issues.empty() && rejected.mutation.empty(), "already owned gaps reject the whole plan atomically");
    graph = fixture(true, false);
    graph.mutable_node(find(graph, "c10d::allreduce_", "1")).duration = 0;
    rejected = plan(graph);
    require(rejected.issues.contains("call_cost_already_changed") && rejected.mutation.empty(), "HiCache-owned submission cannot be charged again");
    graph = broadcast_fixture({ 0, 1, 2, 3 }, 0, false);
    graph.mutable_event_for_node(find(graph, "gloo:broadcast", "100")).dur = 0;
    graph.mutable_event_for_node(find(graph, "gloo:broadcast", "101")).ts += 1;
    rejected = plan(graph);
    require(rejected.issues.contains("inconsistent_call_timing") && rejected.mutation.empty(),
            "broadcast cannot return before a direct receiver entered; no clamped negative remainder");
}
