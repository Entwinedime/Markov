/** @file Phase timing and oracle-component regressions, validation builds only. */
#include "markov/trace_graph/modules/hicache/dag_patch_module.hpp"
#include "markov/trace_graph/simulation/topological_simulator.hpp"
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>

using namespace markov::trace_graph;
using namespace markov::trace_graph::modules::hicache;

namespace {
void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

void phase_operators_preserve_interleaving() {
    core::DagGraph graph;
    auto add = [&](const char* name, bool cpu, uint64_t duration) {
        return graph.add_synthetic_node({.name=name, .is_cpu=cpu, .duration=duration, .counts_toward_e2e=true});
    };
    const auto first = add("first submit", true, 1);
    const auto middle = add("second compute submit", true, 1);
    const auto last = add("last collective submit", true, 1);
    const auto decode = add("decode submit", true, 1);
    const auto common1 = add("layer 1 compute", false, 10);
    const auto comm1 = add("layer 1 collective", false, 5);
    const auto common2 = add("layer 2 compute", false, 10);
    const auto comm2 = add("layer 2 collective", false, 5);
    const auto decode_kernel = add("decode compute", false, 1);
    const auto decode_comm = add("decode collective", false, 1);
    graph.mutable_node(first).cpu_gap_after = 19;
    graph.mutable_node(middle).cpu_gap_after = 9;
    graph.add_edge(first, middle, core::DagEdgeKind::Sequential);
    graph.add_edge(middle, last, core::DagEdgeKind::Sequential);
    graph.add_edge(last, decode, core::DagEdgeKind::Sequential);
    graph.add_edge(first, common1, core::DagEdgeKind::Correlation);
    graph.add_edge(common1, comm1, core::DagEdgeKind::Stream);
    graph.add_edge(comm1, common2, core::DagEdgeKind::Stream);
    graph.add_edge(middle, common2, core::DagEdgeKind::Correlation);
    graph.add_edge(common2, comm2, core::DagEdgeKind::Stream);
    graph.add_edge(last, comm2, core::DagEdgeKind::Correlation);
    graph.add_edge(comm2, decode, core::DagEdgeKind::Sync);
    graph.add_edge(decode, decode_kernel, core::DagEdgeKind::Correlation);
    graph.add_edge(decode_kernel, decode_comm, core::DagEdgeKind::Stream);
    const auto original = simulation::run_topological_simulation(graph).e2e_us;
    require(original == 39, "fixture has 36 us Prefill followed by 3 us Decode");
    model::HiCachePhaseWorkLedger work;
    work.status = work.cost_status = "ready";
    model::HiCachePrefillWorkItem prefill;
    prefill.pid = "1"; prefill.request_id = "request"; prefill.logical_input = 0;
    prefill.common_kernel_cost = {20,20,{common1,common2}};
    prefill.collective_cost = {10,10,{comm1,comm2}};
    prefill.submit_cost = {3,3,{first,middle,last}};
    model::HiCacheDecodeWorkItem dec;
    dec.pid = "1"; dec.request_id = "request"; dec.logical_input = 0;
    dec.kernel_cost = {1,1,{decode_kernel}};
    dec.collective_cost = {1,1,{decode_comm}};
    dec.submit_cost = {1,1,{decode}};
    work.prefills.push_back(prefill); work.decodes.push_back(dec);
    const auto replay = [&](const core::DagGraph& source, const model::HiCachePhaseWorkLedger& costs) {
        auto target = source;
        core::DagMutationPlan plan{.component="phase_test"};
        const auto audit = append_hicache_phase_carrier_plan(target,costs,plan);
        require(audit.status == "ready", "observed operators supply the phase structure");
        const auto mutation = core::apply_dag_mutation_plan(target,plan);
        const auto expected = simulation::run_topological_simulation(target).e2e_us;
        auto live = source;
        std::map<size_t,uint64_t> durations;
        const auto actual = simulation::run_topological_simulation(live,
            [&](size_t node,uint64_t,uint64_t duration) { return durations.contains(node) ? durations.at(node) : duration; },
            [&](size_t node,uint64_t,simulation::FutureDag&) {
                if(node != first) return;
                core::DagMutationPlan operators;
                const auto single = append_hicache_phase_operator_costs(live,costs.prefills.front(),costs.decodes.front(),operators);
                require(single.status == "ready" && operators.synthetic_nodes.empty() && operators.add_edges.empty(),
                        "admission-time projection reuses operator costs without rebuilding phase topology");
                for(const auto& update:operators.set_node_durations) durations.emplace(update.node_id,update.duration);
                require(append_hicache_phase_operator_costs(live,costs.prefills.front(),costs.decodes.front(),operators).status == "blocked",
                        "one operator cannot be owned twice by repeated admission");
            }).e2e_us;
        require(actual == expected && simulation::run_topological_simulation(live).e2e_us == expected,
                "live phase costs and subsequent static replay must match the existing phase projection");
        auto wrong = costs.decodes.front(); wrong.request_id = "another request";
        core::DagMutationPlan mismatch;
        require(append_hicache_phase_operator_costs(source,costs.prefills.front(),wrong,mismatch).status == "blocked"
                && mismatch.set_node_durations.empty(), "request identity cannot cross phase cost ownership");
        return expected;
    };
    require(replay(graph,work) == original, "same costs retain interleaved submission, compute and communication");
    auto pipelined = graph;
    const auto h2d_first = pipelined.add_synthetic_node({.name="layer 1 H2D", .is_cpu=false, .duration=5});
    const auto h2d_last = pipelined.add_synthetic_node({.name="layer 2 H2D", .is_cpu=false, .duration=100});
    pipelined.add_edge(h2d_first,h2d_last,core::DagEdgeKind::Stream);
    pipelined.add_edge(h2d_first,common1,core::DagEdgeKind::Sync);
    pipelined.add_edge(h2d_last,common2,core::DagEdgeKind::Sync);
    patch::HiCacheShadowRewriteTransaction shadow;
    patch::HiCacheRewriteDecision loadback;
    loadback.effect_type = model::HiCacheEffectType::Loadback;
    loadback.cache_scope = "scope:1"; loadback.request_id = "request";
    loadback.source_readiness_topology_reused = true;
    loadback.owned_duration_nodes = {h2d_first,h2d_last};
    shadow.decisions.push_back(loadback);
    for (const auto duration : {100, 300}) {
        auto target = pipelined;
        target.set_node_duration(h2d_last,duration);
        const auto source_time = simulation::run_topological_simulation(target).e2e_us;
        core::DagMutationPlan combined{.component="hicache"};
        require(append_hicache_phase_carrier_plan(target,work,combined).status == "ready"
                && append_hicache_reused_loadback_dependencies(work,shadow,combined), "combined I/O and phase boundaries are supported");
        (void)core::apply_dag_mutation_plan(target,combined);
        require(simulation::run_topological_simulation(target).e2e_us == source_time && target.node(common1).completion_time == 15,
                "later-layer H2D cannot block earlier-layer compute through a whole-Prefill barrier");
        target = pipelined;
        target.set_node_duration(h2d_last,duration);
        core::DagMutationPlan after_prefetch{.component="hicache"};
        after_prefetch.synthetic_nodes.push_back({.synthetic_id="prefetch", .node={.name="prefetch", .is_cpu=false, .duration=50},
                                                .effect_id="prefetch"});
        auto with_prefetch = shadow;
        patch::HiCacheRewriteDecision prefetch;
        prefetch.effect_type = model::HiCacheEffectType::PrefetchIo;
        prefetch.cache_scope = "scope:1"; prefetch.request_id = "request"; prefetch.synthetic_id = "prefetch";
        with_prefetch.decisions.push_back(prefetch);
        require(append_hicache_phase_carrier_plan(target,work,after_prefetch).status == "ready"
                && append_hicache_reused_loadback_dependencies(work,with_prefetch,after_prefetch), "prefetch precedes reused loadback");
        (void)core::apply_dag_mutation_plan(target,after_prefetch);
        require(simulation::run_topological_simulation(target).e2e_us == source_time + 50 && target.node(common1).completion_time == 65,
                "removing the whole-Prefill barrier must not remove upstream prefetch readiness");
    }
    auto larger = work;
    larger.prefills[0].common_kernel_cost.predicted_duration_us = 40;
    require(replay(graph,larger) == 54, "longer device work propagates to the true CPU synchronization");
    auto smaller = work;
    smaller.prefills[0].common_kernel_cost.predicted_duration_us = 10;
    require(replay(graph,smaller) == 39, "faster compute still waits for the last collective submission");
    auto late = graph;
    late.mutable_node(middle).cpu_gap_after = 29;
    require(replay(late,work) == 59, "a late final submission delays only its dependent work");

    auto overlap = graph;
    for (size_t i=0; i<overlap.edge_count(); ++i)
        if (overlap.edge(i).src == comm2 && overlap.edge(i).dst == decode) overlap.mutable_edge(i).active = false;
    overlap.add_edge(comm2,decode_kernel,core::DagEdgeKind::Stream);
    require(simulation::run_topological_simulation(overlap).e2e_us == 38 && replay(overlap,work) == 38,
            "Decode CPU preparation may precede Prefill completion when the actual dependency is on device execution");
    auto missing = work;
    missing.prefills[0].prefix_attention_cost.predicted_duration_us = 1;
    core::DagMutationPlan unsupported;
    const auto audit = append_hicache_phase_carrier_plan(graph,missing,unsupported);
    require(audit.status == "blocked" && audit.blockers.contains("phase_operator_template_missing"),
            "a nonzero cost cannot invent the location of an unobserved operator family");
}

void decode_cost_keeps_attention_on_its_operators() {
    core::DagGraph graph;
    const auto add = [&](const char* name, bool cpu, uint64_t duration) {
        return graph.add_synthetic_node({.name=name, .is_cpu=cpu, .duration=duration, .counts_toward_e2e=true});
    };
    const auto pre_submit = add("prefill submit", true, 1);
    const auto pre_kernel = add("prefill common", false, 1);
    const auto pre_comm = add("prefill collective", false, 1);
    const auto dec_submit = add("decode submit", true, 1);
    const auto common = add("decode common", false, 10);
    const auto attention = add("FusedInferAttentionScore", false, 10);
    const auto consumer = add("CPU consumer of common result", true, 50);
    graph.add_edge(pre_submit, pre_kernel, core::DagEdgeKind::Correlation);
    graph.add_edge(pre_kernel, pre_comm, core::DagEdgeKind::Stream);
    graph.add_edge(pre_comm, dec_submit, core::DagEdgeKind::Sync);
    graph.add_edge(dec_submit, common, core::DagEdgeKind::Correlation);
    graph.add_edge(common, attention, core::DagEdgeKind::Stream);
    graph.add_edge(common, consumer, core::DagEdgeKind::Sync);
    model::HiCachePhaseWorkLedger work;
    work.status = work.cost_status = "ready";
    model::HiCachePrefillWorkItem pre;
    pre.pid = "1"; pre.request_id = "request"; pre.logical_input = 0;
    pre.common_kernel_cost = {1,1,{pre_kernel}};
    pre.collective_cost = {1,1,{pre_comm}};
    pre.submit_cost = {1,1,{pre_submit}};
    work.prefills.push_back(pre);
    model::HiCacheDecodeWorkItem dec;
    dec.pid = "1"; dec.request_id = "request"; dec.logical_input = 0;
    dec.source_paged_attention_duration_us = 10;
    dec.kernel_cost = {20,20,{common,attention}};
    dec.submit_cost = {1,1,{dec_submit}};
    work.decodes.push_back(dec);
    const auto check = [&](uint64_t common_us, uint64_t attention_us, uint64_t expected_us) {
        work.decodes[0].kernel_cost.predicted_duration_us = common_us + attention_us;
        work.decodes[0].predicted_paged_attention_duration_us = attention_us;
        auto target = graph;
        core::DagMutationPlan plan{.component="decode_cost"};
        require(append_hicache_phase_carrier_plan(target,work,plan).status == "ready", "Decode components are supported");
        const auto mutation = core::apply_dag_mutation_plan(target,plan);
        require(target.node(common).duration == common_us && target.node(attention).duration == attention_us,
                "each cost component stays on its own operators, including oracle common costs");
        require(simulation::run_topological_simulation(target).e2e_us == expected_us, "intermediate synchronization sees the correct cost");
    };
    check(10,10,64);
    check(10,30,64); // Scaling the whole Decode would incorrectly produce 74.
    check(10,0,64);
    check(30,5,84); // An oracle can change both components independently.
    const auto replay_oracle = [&](const char* attention_field, bool valid) {
        const std::unique_ptr<FILE, decltype(&std::fclose)> fixture(std::tmpfile(), std::fclose);
        require(fixture != nullptr, "phase oracle fixture must open");
        std::fprintf(fixture.get(), R"({"phase_costs":[
            {"effect_id":"hicache_phase:request:prefill:0:common_kernel","duration_us":1},
            {"effect_id":"hicache_phase:request:prefill:0:prefix_attention","duration_us":0},
            {"effect_id":"hicache_phase:request:prefill:0:collective","duration_us":1},
            {"effect_id":"hicache_phase:request:decode:0:kernel","duration_us":35%s},
            {"effect_id":"hicache_phase:request:decode:0:collective","duration_us":0}]})", attention_field);
        std::fflush(fixture.get());
        auto model_result = std::make_shared<model::HiCacheModelResult>();
        model_result->replay_complete = true;
        model_result->effect_decisions.status = "ready";
        model_result->phase_cost_model.enabled = true;
        model_result->phase_work = work;
        model_result->phase_work.decodes[0].predicted_paged_attention_duration_us = 10;
        auto target = graph;
        HiCacheDagPatchModule module(model_result,true,"", "/proc/self/fd/" + std::to_string(::fileno(fixture.get())));
        try {
            module.apply(target);
            require(valid, "invalid attention oracle must be rejected");
            require(module.result().phase_oracle_cost_replay.status == "ready", "oracle binds exact phase costs");
            require(module.result().apply_blockers.contains("io_resource_plan_not_ready"),
                    "this parser fixture deliberately has no I/O plan; it does not test full patch application");
        } catch (const std::invalid_argument&) {
            require(!valid, "valid phase oracle must not fail");
        }
    };
    replay_oracle(",\"paged_attention_duration_us\":5",true);
    replay_oracle("",false);
    replay_oracle(",\"paged_attention_duration_us\":36",false);
    work.decodes[0].predicted_paged_attention_duration_us = 36;
    core::DagMutationPlan invalid;
    require(append_hicache_phase_carrier_plan(graph,work,invalid).blockers.contains("decode_attention_exceeds_kernel_cost"),
            "attention cannot exceed the total kernel cost");
    work.decodes[0].kernel_cost = {10,40,{common}};
    work.decodes[0].source_paged_attention_duration_us = 0;
    work.decodes[0].predicted_paged_attention_duration_us = 30;
    core::DagMutationPlan missing;
    require(append_hicache_phase_carrier_plan(graph,work,missing).blockers.contains("phase_operator_template_missing"),
            "non-attention operators cannot stand in for an absent attention template");
}

} // namespace

void check_hicache_phase_timing() {
    phase_operators_preserve_interleaving();
    decode_cost_keeps_attention_on_its_operators();
}
