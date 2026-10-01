#include "markov/trace_graph/modules/hicache/runtime/window.hpp"
#include "markov/trace_graph/modules/hicache/execution_boundaries.hpp"
#include "markov/trace_graph/modules/hicache/phase_carrier.hpp"
#include "markov/trace_graph/modules/hicache/runtime/layer_calls.hpp"
#include "markov/trace_graph/modules/hicache/runtime/lifecycle_calls.hpp"
#include "markov/trace_graph/modules/hicache/runtime/loads.hpp"
#include "markov/trace_graph/modules/hicache/runtime/prefetch_queries.hpp"
#include "markov/trace_graph/modules/hicache/runtime/prefetch_waits.hpp"
#include "markov/trace_graph/modules/hicache/runtime/preparation.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_calls.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_confirmations.hpp"
#include <algorithm>
#include <set>
#include <stdexcept>

namespace markov::trace_graph::modules::hicache::runtime {

HiCacheWindowResult execute_hicache_window(core::DagGraph & graph, const frontend::HiCacheConfig & config, std::string_view source_policy,
                                           const core::ClientRequestChain & chain, uint64_t begin, uint64_t end) {
    if (chain.status != "connected" || chain.requests.empty() || end <= begin)
        throw std::invalid_argument("HiCache execution requires a connected serial HTTP window");
    HiCacheWindowResult output;
    model::HiCacheModelReplay replay(graph, config, true);
    replay.prepare_decode_allocations(graph, begin, end);
    output.prepared_facts = replay.facts().size();
    const auto bound = bind_hicache_execution_boundaries(graph, replay.facts());
    if (!bound.issues.empty()) throw std::runtime_error("HiCache execution has unbound state facts");
    std::map<size_t, std::vector<size_t>> facts_at;
    for (const auto & item : replay.facts()) facts_at[bound.fact_nodes.at(item.fact.source_node_id)].push_back(item.fact.source_node_id);
    HiCachePrefetchQueries queries;
    HiCachePrefetchWaits waiting;
    HiCacheWriteStreamInsertions stream_insertions;
    stream_insertions.bind_synchronizations(graph);
    HiCacheLoads loads(stream_insertions, config);
    HiCacheLayerCalls layer_calls(stream_insertions, config.layer_wait_calibration);
    HiCacheQueueConfirmations confirmations;
    HiCacheWriteCalls writes(replay, config, stream_insertions);
    HiCacheWriteConfirmations write_confirmations;
    queries.bind(graph, replay, config, facts_at, begin, end);
    output.query_calibration_manifest = queries.source_template().calibration_manifest;
    waiting.bind(graph, replay, source_policy, config.prefetch_policy, chain, config.prefetch_wait_calibration, config.prefetch_cpu_calibrations);
    output.wait_template_request = waiting.source_template().request_id;
    output.wait_calibration_manifest = waiting.source_template().calibration_manifest;
    output.local_return_calibration_manifests = waiting.source_template().local_return_calibration_manifests;
    for (const auto & [node, ids] : facts_at)
        if (!ids.empty() && !graph.node(node).active) throw std::runtime_error("Wait replacement removed a state fact boundary");
    loads.bind(graph, replay, begin, end);
    layer_calls.bind(graph, begin, end);
    HiCacheLifecycleCalls lifecycles;
    lifecycles.bind(graph, replay, begin, end);
    for (auto & [node, ids] : facts_at) std::erase_if(ids, [&](size_t id) { return lifecycles.owns(id); });
    if (!config.write_confirmation_calibration.empty())
        write_confirmations.prepare(graph, config.write_confirmation_calibration, begin, end, waiting.replaced_intervals(), chain.hicache_idle_since_us);
    writes.bind(graph, begin, end);
    HiCacheQueueConfirmations::WriteWork confirmation_work;
    if (!config.write_confirmation_calibration.empty()) {
        write_confirmations.replace_source_tails(graph);
        confirmation_work = [&](const WriteConfirmationWork & work, simulation::FutureDag & future) {
            return write_confirmations.expand(work, replay.state(), writes, future);
        };
    }
    for (const auto & [node, ids] : facts_at)
        if (!ids.empty() && !graph.node(node).active) throw std::runtime_error("Eviction replacement removed a state input boundary");
    confirmations.bind(graph,
                       replay,
                       begin,
                       end,
                       waiting.replaced_intervals(),
                       confirmation_work,
                       chain.hicache_idle_since_us,
                       !config.load_submission_calibration.empty());
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
    const auto preparation_source = observe_allocator_preparations(graph);
    const auto preparation_slots = bind_allocator_preparation_costs(graph, preparation_source);
    // No source regions are replaced after this point. Every generated host
    // operation binds against the same surviving worker queues.
    {
        const patch::HiCacheSourceDagIndex retained(graph);
        const auto workers = observe_host_worker_members(graph);
        loads.rebind_retained_resources(retained, workers);
        layer_calls.rebind_workers(graph, workers);
        write_confirmations.rebind_workers(graph, workers);
        writes.rebind_retained_resources(retained, workers);
    }
    std::map<size_t, size_t> preparation_at;
    for (const auto & [call, node] : preparation_slots) preparation_at.emplace(node, call);
    std::map<size_t, uint64_t> preparation_costs, operator_costs;
    std::set<size_t> started;
    std::set<std::pair<std::string, std::string>> projected;
    size_t admitted_prefills = 0;
    const HiCacheLayerCalls::Consumer layer_consumer = [&](const std::string & pid, uint64_t batch) -> const std::vector<size_t> & {
        return loads.layer_consumer(pid, batch);
    };
    const auto complete_fact = [&](const model::HiCacheReplayFact & input) {
        ++output.consumed_facts;
        if (input.role == HiCacheFactRole::CacheDecodeAllocation) {
            ++output.decode_allocations;
            return;
        }
        const auto & phase_work = replay.current_phase_work();
        if (!phase_work.blockers.empty()) throw std::runtime_error("Phase input incomplete: " + phase_work.blockers.begin()->first);

        if (input.role == HiCacheFactRole::CacheExtendInput) {
            const auto preparation = predict_allocator_preparations(preparation_source, phase_work.allocator_calls);
            if (preparation.status != "ready") throw std::runtime_error("Preparation cost is not covered by base observations");
            for (const auto & cost : preparation.cpu_costs) {
                const auto slot = preparation_slots.find(cost.source_call);
                if (slot == preparation_slots.end()) {
                    if (cost.duration_us || !preparation_source.calls.at(cost.source_call).intervals.empty())
                        throw std::runtime_error("Target preparation lacks its submission site");
                    continue;
                }
                if (started.contains(slot->second) && preparation_costs.at(slot->second) != cost.duration_us)
                    throw std::runtime_error("Later target history revised an executed preparation cost");
                preparation_costs[slot->second] = cost.duration_us;
            }
        }

        for (; admitted_prefills < phase_work.prefills.size(); ++admitted_prefills) {
            const auto & prefill = phase_work.prefills[admitted_prefills];
            const auto key = std::pair{ prefill.pid, prefill.request_id };
            if (projected.contains(key)) continue;
            const auto & decode = phase_work.decodes[admitted_prefills];
            core::DagMutationPlan costs;
            const auto audit = append_hicache_phase_operator_costs(graph, prefill, decode, costs);
            if (audit.status != "ready") throw std::runtime_error("Phase cost projection failed");
            for (const auto & cost : costs.set_node_durations) {
                // Unchanged CPU work may already have executed. A no-op is
                // not a retroactive update; actual changes are.
                if (cost.duration == graph.node(cost.node_id).duration) continue;
                if (started.contains(cost.node_id)) throw std::runtime_error("Phase admitted after an operator already started");
                if (!operator_costs.emplace(cost.node_id, cost.duration).second) throw std::runtime_error("Duplicate phase ownership");
            }
            projected.insert(key);
        }
    };
    output.simulation = simulation::run_topological_simulation(
        graph,
        [&](size_t node, uint64_t, uint64_t duration) {
            if (preparation_at.contains(node)) {
                if (!preparation_costs.contains(node)) throw std::runtime_error("Preparation started before its target demand was known");
                ++output.executed_preparations;
                return preparation_costs.at(node);
            }
            return writes.duration(node, operator_costs.contains(node) ? operator_costs.at(node) : duration);
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
            if (const auto at = facts_at.find(node); at != facts_at.end())
                for (const auto id : at->second) {
                    const auto & input = replay.fact(id);
                    replay.apply(id, time, node);
                    writes.applied(input);
                    if (input.consumed) complete_fact(input);
                }
            loads.advance(node, time, future);
            layer_calls.advance(node, layer_consumer, future);
            queries.advance(node, time, future);
            started.insert(node);
        });
    output.model = replay.finish();
    if (!output.model.effect_decisions.missing_facts.empty()) throw std::runtime_error("HiCache execution finished with missing model facts");
    output.phase_admissions = projected.size();
    output.preparation_slots = preparation_slots.size();
    output.changed_operators = operator_costs.size();
    output.source_load_submissions = loads.source_submissions();
    output.load_submission_base_cost_batches = loads.base_cost_batches();
    output.load_submission_shared_cost_batches = loads.shared_cost_batches();
    output.completed_load_batches = loads.completed_batches();
    output.prepared_layer_calls = layer_calls.prepared_calls();
    output.active_layer_calls = layer_calls.active_calls();
    output.inactive_layer_calls = layer_calls.inactive_calls();
    output.source_write_submissions = writes.source_submissions();
    output.completed_writes = writes.completed_writes();
    output.resumed_allocations = writes.resumed_allocations();
    output.prepared_write_templates = writes.prepared_templates();
    output.prepared_eviction_controls = writes.prepared_eviction_controls();
    output.expanded_write_submissions = writes.expanded_submissions();
    output.calibrated_empty_checks = writes.calibrated_empty_checks();
    output.generated_control_steps = writes.generated_control_steps();
    output.generated_blocking_checks = writes.generated_blocking_checks();
    output.blocking_check_cost_extrapolations = writes.blocking_check_cost_extrapolations();
    output.phase_extrapolated_control_steps = writes.phase_extrapolated_control_steps();
    output.calibrated_empty_check_cpu_us = writes.calibrated_empty_check_cpu_us();
    output.confirmations = confirmations.coverage();
    const auto endpoint = chain.requests.back().completion;
    output.http_us = graph.node(endpoint).completion_time;

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
