#include "markov/trace_graph/modules/hicache/runtime/window.hpp"
#include "markov/trace_graph/cost/hicache_io.hpp"
#include "markov/trace_graph/cost/hicache_phase.hpp"
#include "markov/trace_graph/modules/hicache/execution_boundaries.hpp"
#include "markov/trace_graph/modules/hicache/phase_carrier.hpp"
#include "markov/trace_graph/modules/hicache/runtime/allocator_calls.hpp"
#include "markov/trace_graph/modules/hicache/runtime/collective_services.hpp"
#include "markov/trace_graph/modules/hicache/runtime/layer_calls.hpp"
#include "markov/trace_graph/modules/hicache/runtime/lifecycle_calls.hpp"
#include "markov/trace_graph/modules/hicache/runtime/loads.hpp"
#include "markov/trace_graph/modules/hicache/runtime/phase_calls.hpp"
#include "markov/trace_graph/modules/hicache/runtime/prefetch_queries.hpp"
#include "markov/trace_graph/modules/hicache/runtime/prefetch_waits.hpp"
#include "markov/trace_graph/modules/hicache/runtime/preparation.hpp"
#include "markov/trace_graph/modules/hicache/runtime/storage_writes.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_calls.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_confirmations.hpp"
#include "markov/trace_graph/source_dag/phase_calls.hpp"
#include "markov/trace_graph/structure/hicache/phase.hpp"
#include "markov/trace_graph/structure/hicache/resources.hpp"
#include "markov/trace_graph/structure/missing_work.hpp"
#include <algorithm>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <utility>

namespace markov::trace_graph::modules::hicache::runtime {

HiCacheWindowResult execute_hicache_window(core::DagGraph & graph, const core::DagGraph & source_graph, const frontend::HiCacheConfig & config,
                                           std::string_view source_policy, const core::ClientRequestChain & chain, uint64_t begin, uint64_t end,
                                           const simulation::NodeCostAtStart & final_cost_transform) {
    if (chain.status != "connected" || chain.requests.empty() || end <= begin)
        throw std::invalid_argument("HiCache execution requires a connected serial HTTP window");
    HiCacheWindowResult output;
    output.final_cost_transform_enabled = static_cast<bool>(final_cost_transform);
    model::HiCacheModelReplay replay(source_graph,
                                     config,
                                     cost::io_completion_provider(config.io_cost),
                                     config.io_cost.storage_batch_pages,
                                     config.io_cost.resource_lanes,
                                     true);
    replay.prepare_decode_allocations(source_graph, begin, end);
    const auto source_calls = [&] {
        const patch::HiCacheSourceDagIndex source(source_graph);
        const auto queues = simulation::detail::discover_cpu_task_queues(source_graph);
        return source_dag::observe_phase_calls(source, queues, replay.source_phases());
    }();
    output.phase_observation_issues = source_calls.issues();
    if (!output.phase_observation_issues.empty())
        throw structure::MissingWork("Phase replacement needs complete per-call source observations", output.phase_observation_issues);

    output.prepared_facts = replay.facts().size();
    const auto bound = bind_hicache_execution_boundaries(graph, replay.facts());
    if (!bound.issues.empty()) {
        auto gaps = nlohmann::json::array();
        for (const auto & [fact, issues] : bound.issues)
            gaps.push_back({
                { "source_fact",   fact },
                {      "issues", issues }
            });

        throw structure::MissingWork("HiCache state facts lack execution boundaries", std::move(gaps));
    }
    std::map<size_t, std::vector<size_t>> facts_at;
    for (const auto & item : replay.facts()) facts_at[bound.fact_nodes.at(item.fact.source_node_id)].push_back(item.fact.source_node_id);
    HiCachePrefetchQueries queries;
    HiCachePrefetchWaits waiting;
    HiCacheWriteStreamInsertions stream_insertions;
    stream_insertions.bind_synchronizations(graph);
    const auto compute_resources = structure::hicache::observe_scheduler_compute_resources(patch::HiCacheSourceDagIndex(graph));
    cost::OperatorCostProvider operator_provider(config.operator_models);
    HiCacheLoads loads(stream_insertions, config, compute_resources.schedule, operator_provider);
    HiCacheLayerCalls layer_calls(stream_insertions, operator_provider);
    HiCachePhaseCalls phase_calls(layer_calls, stream_insertions, compute_resources.forward, operator_provider);
    HiCacheCollectiveServices collective_services;
    HiCacheAllocatorCalls allocator_calls(stream_insertions, operator_provider);
    HiCacheQueueConfirmations confirmations(operator_provider);
    HiCacheWriteCalls writes(replay, config, stream_insertions, compute_resources.schedule, operator_provider);
    HiCacheWriteConfirmations write_confirmations(operator_provider, config.page_size, compute_resources.schedule);
    HiCacheStorageWrites storage_writes(replay.state(), config);
    queries.bind(graph, replay, config, operator_provider, facts_at);
    waiting.bind(graph, replay, source_policy, config.prefetch_policy, chain, operator_provider);
    for (const auto & [node, ids] : facts_at)
        if (!ids.empty() && !graph.node(node).active) throw std::runtime_error("Wait replacement removed a state fact boundary");
    loads.bind(graph, replay, begin, end);
    layer_calls.bind(graph, begin, end);
    HiCacheLifecycleCalls lifecycles;
    lifecycles.bind(graph, replay, begin, end, chain.requests.front().start);
    for (auto & [node, ids] : facts_at) std::erase_if(ids, [&](size_t id) { return lifecycles.owns(id); });
    write_confirmations.prepare(graph, begin, end, waiting.replaced_intervals(), chain.hicache_idle_since_us);
    // Confirmation tails can synchronize source write Records even after the
    // nonblocking readiness check succeeds. Replace those consumers before
    // replacing write producers. Target acknowledgement dependencies are
    // generated separately from target completion/publication state.
    write_confirmations.replace_source_tails(graph);
    writes.bind(graph, begin, end, chain.requests.front().start);
    const HiCacheQueueConfirmations::WriteWork confirmation_work = [&](const WriteConfirmationWork & work, simulation::FutureDag & future) {
        return write_confirmations.expand(work, replay.state(), writes, future);
    };
    for (const auto & [node, ids] : facts_at)
        if (!ids.empty() && !graph.node(node).active) throw std::runtime_error("Eviction replacement removed a state input boundary");
    confirmations.bind(graph, replay, begin, end, waiting.replaced_intervals(), confirmation_work, chain.hicache_idle_since_us);
    confirmations.replace_load_tails(graph);
    writes.replace_active_load_branches(graph);
    loads.replace_source_submissions(graph);
    {
        const patch::HiCacheSourceDagIndex source(graph);
        core::DagMutationPlan plan{ .component = "cpu_collective" };
        output.cpu_collectives = patch::append_retained_cpu_collectives(source, observe_cpu_collectives(source), plan);
        if (output.cpu_collectives.status == "blocked") throw std::runtime_error("CPU collective composition failed");
        const auto mutation = core::apply_dag_mutation_plan(graph, plan);
#ifdef DEBUG
        if (!mutation.topology.ok()) throw std::runtime_error("CPU collective topology invalid");
#endif
    }
    collective_services.bind(graph, source_graph, source_calls.collectives.calls, begin, end);
    phase_calls.bind(graph, source_graph, source_calls.attention.calls, source_calls.metadata.calls, source_calls.dense.calls, begin, end);
    const auto preparation_source = observe_allocator_preparations(source_graph);
    allocator_calls.bind(graph, source_graph, replay.facts(), compute_resources.schedule, preparation_source, begin, end);
    // No source regions are replaced after this point. Every generated host
    // operation binds against the same surviving worker queues.
    {
        const patch::HiCacheSourceDagIndex retained(graph);
        const auto workers = observe_host_worker_members(graph);
        loads.rebind_retained_resources(retained, workers);
        layer_calls.rebind_retained_resources(retained, workers);
        phase_calls.rebind_retained_resources(retained, workers);
        allocator_calls.rebind_resources(retained, workers);
        writes.rebind_retained_resources(retained, workers);
        write_confirmations.rebind_resources(workers);
    }
    std::map<size_t, uint64_t> operator_costs;
    std::set<size_t> phase_cost_migrations;
    std::set<size_t> started;
    size_t admitted_prefills = 0;
    size_t admitted_allocations = 0;
    const HiCacheLayerCalls::Consumer layer_consumer = [&](const std::string & pid, uint64_t batch) -> const std::vector<size_t> & {
        return loads.layer_consumer(pid, batch);
    };
    const auto complete_fact = [&](const model::HiCacheReplayFact & input) {
        ++output.consumed_facts;
        const auto & phase_work = replay.current_phase_work();
        if (admitted_allocations < phase_work.allocator_calls.size()) {
            const auto preparation = structure::hicache::plan_allocator_work(preparation_source.supported_pids, phase_work.allocator_calls);
            if (preparation.status != "ready") {
                nlohmann::json gaps = preparation.blockers;
                for (size_t i = 0; i < preparation.calls.size(); ++i) {
                    const auto & call = phase_work.allocator_calls[i];
                    const auto & status = preparation.calls[i].status;
                    if (!call.formal || (status != "unknown" && status != "possibly_required" && status != "unsupported_signature")) continue;

                    gaps["first_unresolved_call"] = {
                        {       "source_fact_id",                                                                        call.source_fact_id },
                        {                  "pid",                                                                                   call.pid },
                        {               "status",                                                                                     status },
                        {     "page_size_tokens",                                                                             call.page_size },
                        {           "batch_size",                                                                            call.batch_size },
                        {        "extend_tokens",                                                                         call.extend_tokens },
                        {      "allocated_pages",                                                                       call.allocated_pages },
                        {    "free_pages_before",                                                                     call.free_pages_before },
                        { "release_pages_before",                                                                  call.release_pages_before },
                        {  "merge_release_queue",                                                                   call.merge_release_queue },
                        {    "free_index_offset", call.free_index_offset ? nlohmann::json(*call.free_index_offset) : nlohmann::json(nullptr) },
                    };
                    break;
                }
                throw structure::MissingWork("Allocator preparation work has unresolved target semantics", std::move(gaps));
            }
            for (; admitted_allocations < phase_work.allocator_calls.size(); ++admitted_allocations)
                allocator_calls.set_work(phase_work.allocator_calls[admitted_allocations], preparation.calls[admitted_allocations]);
        }
        if (input.role == HiCacheFactRole::CacheDecodeAllocation) {
            ++output.decode_allocations;
            return;
        }
        if (!phase_work.blockers.empty()) throw structure::MissingWork("Phase input incomplete", phase_work.blockers);

        // The source join admits one phase per request/rank. This cursor
        // consumes each appended work item once; no second identity set is needed.
        for (; admitted_prefills < phase_work.prefills.size(); ++admitted_prefills) {
            const auto & prefill = phase_work.prefills[admitted_prefills];
            const auto & decode = phase_work.decodes[admitted_prefills];
            const auto source_phase_index = replay.source_phase_index(prefill.pid, prefill.request_id);
            const auto & source_phase = replay.source_phases().observations.at(source_phase_index);
            auto work = structure::hicache::plan_phase_calls(source_calls, source_phase_index, prefill, decode);
            for (auto & call : work.tensors) {
                output.phase_tensor_work.push_back(std::move(call));
                phase_calls.set_work(output.phase_tensor_work.back());
            }
            for (auto & call : work.collectives) {
                output.collective_work.push_back(std::move(call));
                collective_services.set_work(output.collective_work.back());
            }

            auto operations = structure::hicache::phase_migration_candidates(source_graph, source_phase, prefill, decode);
            std::erase_if(operations, [&](const auto & item) {
                return phase_calls.owns(item.first) || allocator_calls.owns(item.first) || collective_services.owns(item.first);
            });

            structure::hicache::require_complete_phase_migrations(operations);
            const auto costs = cost::bind_phase_cost_migrations(source_graph, operations, output.phase_operator_costs);
            for (const auto & cost : costs) {
                phase_cost_migrations.insert(cost.node_id);

                // Unchanged CPU work may already have executed. A no-op is
                // not a retroactive update; actual changes are.
                const auto bound_duration = started.contains(cost.node_id) && final_cost_transform
                                                ? final_cost_transform(cost.node_id, graph.node(cost.node_id).simulation_start, cost.duration_us)
                                                : cost.duration_us;
                if (bound_duration == graph.node(cost.node_id).duration) continue;
                if (started.contains(cost.node_id)) throw std::runtime_error("Phase admitted after an operator already started");
                if (!operator_costs.emplace(cost.node_id, cost.duration_us).second) throw std::runtime_error("Duplicate phase ownership");
            }
        }
    };
    output.simulation = simulation::run_topological_simulation(
        graph,
        [&](size_t node, uint64_t start_us, uint64_t duration) {
            if (operator_costs.contains(node)) duration = operator_costs.at(node);
            if (const auto cost = collective_services.caller_cost(graph, node, operator_provider)) duration = *cost;
            if (const auto cost = collective_services.worker_cost(graph, node, operator_provider)) duration = *cost;
            if (const auto work = collective_services.demand(graph, node)) {
                const auto estimate = operator_provider.estimate(*work);
                duration = estimate.duration_us;
                auto & event = graph.mutable_event_for_node(node);
                event.set_arg("cost_binding_source",
                              work->timing_scope == "device_service" ? "collective_event_independent_model" : "device_collective_independent_model");
                event.set_arg("cost_operation", work->operation);
                event.set_arg("cost_implementation", work->implementation);
                event.set_arg("cost_timing_scope", work->timing_scope);
            }

            return final_cost_transform ? final_cost_transform(node, start_us, duration) : duration;
        },
        [&](size_t node, uint64_t elapsed, simulation::FutureDag & future) {
            const auto time = begin + elapsed;
            if (node == chain.requests.front().start) {
                queries.start(future);
                waiting.start(future);
            }
            for (const auto id : lifecycles.advance(node, time, replay)) {
                const auto & input = replay.fact(id);
                writes.applied(input);
                if (input.consumed) complete_fact(input);
            }
            for (const auto id : writes.advance(node, time, future)) complete_fact(replay.fact(id));
            waiting.advance(node, time, future);
            confirmations.advance(node, time, future);
            write_confirmations.advance(node, time, replay.state());
            if (const auto at = facts_at.find(node); at != facts_at.end())
                for (const auto id : at->second) {
                    const auto & input = replay.fact(id);
                    replay.apply(id, time, node);
                    writes.applied(input);
                    if (input.consumed) complete_fact(input);
                }
            loads.advance(node, time, future);
            phase_calls.advance(graph, node, layer_consumer, future);
            allocator_calls.advance(graph, node, future);
            layer_calls.advance(node, layer_consumer, future);
            queries.advance(node, time, future);
            storage_writes.advance(node, time, future);
            started.insert(node);
        });
    output.model = replay.finish();
    output.prepared_phase_calls = phase_calls.prepared();
    output.prepared_collective_services = collective_services.prepared();
    output.prepared_collective_events = collective_services.prepared_events();
    output.prepared_collective_worker_segments = collective_services.prepared_workers();
    output.prepared_collective_caller_segments = collective_services.prepared_callers();
    output.executed_phase_calls = phase_calls.executed();
    if (output.executed_phase_calls != output.prepared_phase_calls) throw std::runtime_error("Execution did not expand every prepared attention call");
    output.prepared_allocator_calls = allocator_calls.prepared();
    output.executed_allocator_calls = allocator_calls.executed();
    output.retained_allocator_calls = allocator_calls.retained();
    if (output.executed_allocator_calls != output.prepared_allocator_calls) throw std::runtime_error("Execution did not expand every prepared allocator call");
    if (!output.model.effect_decisions.missing_facts.empty()) throw std::runtime_error("HiCache execution finished with missing model facts");
    output.phase_admissions = admitted_prefills;
    output.changed_operators = operator_costs.size();
    output.source_load_submissions = loads.source_submissions();
    output.load_submission_shared_cost_batches = loads.shared_cost_batches();
    output.completed_load_batches = loads.completed_batches();
    output.prepared_layer_calls = layer_calls.prepared_calls();
    output.active_layer_calls = layer_calls.active_calls();
    output.inactive_layer_calls = layer_calls.inactive_calls();
    output.source_write_submissions = writes.source_submissions();
    output.completed_writes = writes.completed_writes();
    output.resumed_allocations = writes.resumed_allocations();
    output.observed_write_fragments = writes.observed_write_fragments();
    output.observed_eviction_fragments = writes.observed_eviction_fragments();
    output.expanded_write_submissions = writes.expanded_submissions();
    output.generated_control_steps = writes.generated_control_steps();
    output.generated_blocking_checks = writes.generated_blocking_checks();
    output.confirmations = confirmations.coverage();
    const auto endpoint = chain.requests.back().completion;
    output.http_us = graph.node(endpoint).completion_time;
    output.independent_operator_queries = operator_provider.queries();

    // Binding provenance is independent of whether a numerical override was
    // needed. Never infer migration merely from a surviving trace node.
    for (const auto id : started) {
        const auto & node = graph.node(id);
        if (!node.active) continue;

        std::string origin = "unclassified";
        if (phase_cost_migrations.contains(id)) origin = "phase_source_migration";
        else if (node.kind == core::DagNodeKind::Synthetic) origin = graph.event_for_node(id).arg("cost_binding_source", "unclassified");
        if (origin == "device_collective_independent_model") ++output.executed_collective_services;
        if (origin == "collective_event_independent_model") ++output.executed_collective_events;
        if (origin == "collective_worker_independent_model") ++output.executed_collective_worker_segments;
        if (origin == "collective_caller_independent_model") ++output.executed_collective_caller_segments;

        auto & totals = output.executed_cost_sources[origin];
        ++totals.nodes;
        totals.node_duration_us += node.duration;
        totals.sequential_gap_us += node.cpu_gap_after;
        totals.ready_delay_us += node.cpu_ready_delay_before;
    }

#ifdef DEBUG
    // TRACE_GRAPH_DEBUG enables this independent validation replay. Normal
    // prediction uses the execution above, without simulating the DAG twice.
    std::vector<std::pair<uint64_t, uint64_t>> times;
    times.reserve(graph.node_count());
    for (const auto & node : graph.nodes()) times.emplace_back(node.simulation_start, node.completion_time);

    (void)simulation::run_topological_simulation(graph);
    for (const auto & node : graph.nodes())
        if (node.active && times[node.id] != std::pair{ node.simulation_start, node.completion_time })
            throw std::runtime_error("HiCache expanded DAG differs from its executed timing");
#endif

    return output;
}

} // namespace markov::trace_graph::modules::hicache::runtime
