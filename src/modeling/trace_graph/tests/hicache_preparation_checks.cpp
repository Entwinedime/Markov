/** @file Source-derived preparation costs and their real CPU submission point. */
#include "markov/trace_graph/modules/hicache/runtime/preparation.hpp"
#include "markov/trace_graph/simulation/topological_simulator.hpp"
#include <stdexcept>

using namespace markov::trace_graph;
using namespace markov::trace_graph::modules::hicache;

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

core::TraceEvent preparation(uint64_t ts, uint64_t duration, uint64_t tokens, bool load = false) {
    core::TraceEvent event;
    event.name = load ? "runtime.triton.load" : "runtime.triton.prepare";
    event.pid = event.tid = "1"; event.ts = ts; event.dur = duration;
    event.set_arg("kernel", "alloc_extend_kernel");
    event.set_arg("status", "returned");
    event.set_arg("path", "compiled"); event.set_arg("execution_mode", "sync");
    event.set_arg("constants", "{\"page_size\":64,\"bs_upper\":1,\"max_num_extend_tokens\":" + std::to_string(tokens) + ",\"BLOCK_SIZE\":2048}");
    event.set_arg("signature", R"({"pre_lens_ptr":"*i64","seq_lens_ptr":"*i64","last_loc_ptr":"*i64","free_page_ptr":"*i64","out_indices":"*i64"})");
    event.set_arg("argument_properties", R"({"tt.divisibility":[0,1,2,3,4]})");
    return event;
}

core::DagGraph source_graph() {
    core::DagGraph graph;
    const auto add = [&](const char* name, bool cpu, uint64_t ts, uint64_t duration, const char* tid) {
        const auto id = graph.add_synthetic_node({.name=name, .is_cpu=cpu, .duration=duration, .counts_toward_e2e=true});
        auto& event = graph.mutable_event_for_node(id);
        event.pid = "1"; event.tid = tid; event.ts = ts; event.dur = duration;
        return id;
    };
    add("before allocation", true, 1000, 1, "1");
    add("Enqueue@alloc_extend_kernel", true, 1003, 1, "1");
    add("Node@launch", true, 1004, 1, "worker");
    add("alloc_extend_kernel", false, 1005, 5, "stream");
    add("worker previous task", true, 990, 15, "worker");
    graph.mutable_node(0).cpu_gap_after = graph.mutable_node(0).original_cpu_gap_after = 2;
    graph.add_edge(0,1,core::DagEdgeKind::Sequential);
    graph.add_edge(1,2,core::DagEdgeKind::Correlation);
    graph.add_edge(2,3,core::DagEdgeKind::Correlation);
    graph.add_edge(4,2,core::DagEdgeKind::Sequential);
    auto fact = graph.event_for_node(0); fact.index = 0;
    graph.set_hicache_fact_events({fact});
    graph.set_runtime_observations({preparation(10,100,64), preparation(111,200,64,true),
                                   preparation(320,40,128), preparation(361,4,128,true)});
    return graph;
}

model::HiCacheAllocatorWorkItem call(const char* pid, const char* request, uint64_t tokens, bool formal = false) {
    return {.pid=pid, .request_ids={request}, .formal=formal, .page_size=64,
            .batch_size=1, .extend_tokens=tokens, .allocated_pages=1, .free_index_offset=0};
}
}

void check_hicache_preparation_costs() {
    auto graph = source_graph();
    const auto warm = call("1","warm",64), formal = call("1","formal",256,true);
    const auto plan = runtime::plan_allocator_preparations(graph,{warm,formal});
    require(plan.status == "ready" && plan.source_parallel_compilation, "confirmed source path supports the parallel cold approximation");
    require(plan.cost_samples.at("1").at("compiled").median_us == 70
            && plan.cost_samples.at("1").at("variant_load").median_us == 4, "use measured medians, excluding the first runtime load");
    require(plan.added_cost_us == 74 && plan.removed_coverage_us == 0
            && plan.mutation.set_cpu_gaps.size() == 1 && plan.mutation.set_cpu_gaps[0].node_id == 0
            && plan.mutation.set_cpu_gaps[0].duration == 76, "new cost belongs before enqueue, not at the device or phase end");
    require(simulation::run_topological_simulation(graph).e2e_us == 21, "source worker is initially busy");
    const auto applied = core::apply_dag_mutation_plan(graph,plan.mutation);
    require(simulation::run_topological_simulation(graph).e2e_us == 84, "preparation shifts real submission while retaining worker overlap");
    graph = source_graph();
    require(runtime::plan_allocator_preparations(graph,{formal}).blockers.contains("preparation_first_runtime_state_uncovered"),
            "formal first runtime use is not priced with the cheap later load");
    auto missing = graph;
    missing.mutable_edge(2).active = false;
    const auto absent = runtime::plan_allocator_preparations(missing,{warm,formal});
    require(absent.status == "partial" && absent.added_cost_us == 0 && absent.mutation.empty(), "missing submission ancestry must not guess an anchor");
    auto events = graph.runtime_observations();
    for (auto& event : events) event.set_arg("path", "unknown");
    missing.set_runtime_observations(events);
    require(runtime::plan_allocator_preparations(missing,{warm,formal}).blockers.contains("preparation_compilation_context_uncovered"),
            "legacy or unclassified preparation is not a new compile sample");

    // Source preparation removal and a target addition must succeed together.
    auto atomic = source_graph();
    atomic.mutable_node(0).cpu_gap_after = atomic.mutable_node(0).original_cpu_gap_after = 90;
    atomic.mutable_event_for_node(1).ts = 1091;
    auto facts = atomic.hicache_fact_events();
    auto later_fact = facts.front(); later_fact.index = 1; later_fact.ts = 1200;
    facts.push_back(later_fact); atomic.set_hicache_fact_events(facts);
    events = atomic.runtime_observations();
    events.push_back(preparation(1002,30,512)); events.push_back(preparation(1032,4,512,true));
    atomic.set_runtime_observations(events);
    auto reuse = call("1","reuse",64,true), later = formal; later.source_fact_id = 1;
    const auto failed = runtime::plan_allocator_preparations(atomic,{warm,reuse,later});
    require(failed.blockers.contains("preparation_submit_anchor_not_unique") && failed.mutation.empty()
            && failed.removed_coverage_us == 0 && failed.added_cost_us == 0, "do not delete source cost when target addition is uncovered");
    events[events.size() - 2].set_arg("execution_mode", "async");
    atomic.set_runtime_observations(events);
    const auto asynchronous = runtime::plan_allocator_preparations(atomic,{warm,reuse});
    require(asynchronous.blockers.contains("preparation_async_interval_incomplete") && asynchronous.mutation.empty(),
            "an async submission interval cannot stand in for the complete source preparation being removed");

    auto two = source_graph();
    events = two.runtime_observations();
    const auto original_events = events;
    for (auto event : original_events) { event.pid = event.tid = "2"; events.push_back(std::move(event)); }
    two.set_runtime_observations(events);
    std::vector<model::HiCacheAllocatorWorkItem> calls{call("1","a",64),call("2","a",128),
        call("1","b",256),call("2","b",256),call("2","c",64)};
    const auto cache = runtime::plan_allocator_preparations(two,calls);
    require(cache.calls[2].path == "compiled" && cache.calls[3].path == "compiled", "same-batch ranks both compile, independent of iteration order");
    require(cache.calls[4].path == "disk_cache", "a later batch can reuse another rank's completed variant");
    std::swap(calls[2],calls[3]);
    const auto reordered = runtime::plan_allocator_preparations(two,calls);
    require(reordered.calls[2].path == "compiled" && reordered.calls[3].path == "compiled", "rank order is not a cache availability clock");
    calls[0].free_index_offset.reset();
    require(runtime::plan_allocator_preparations(two,calls).calls[2].path == "unknown", "unknown earlier cache history propagates across ranks");
}
