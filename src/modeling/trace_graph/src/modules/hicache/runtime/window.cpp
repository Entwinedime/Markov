#include "markov/trace_graph/modules/hicache/runtime/window.hpp"
#include "core/execution_metrics.hpp"
#include "markov/trace_graph/adapters/frameworks/sglang/allocator_preparation.hpp"
#include "markov/trace_graph/adapters/frameworks/sglang/allocator_program.hpp"
#include "markov/trace_graph/adapters/frameworks/sglang/execution_resources.hpp"
#include "markov/trace_graph/adapters/frameworks/sglang/hicache_observations.hpp"
#include "markov/trace_graph/adapters/frameworks/sglang/source_calls.hpp"
#include "markov/trace_graph/cost/source_migration.hpp"
#include "markov/trace_graph/frontend/model_config.hpp"
#include "markov/trace_graph/modules/hicache/execution_boundaries.hpp"
#include "markov/trace_graph/modules/hicache/model/replay.hpp"
#include "markov/trace_graph/modules/hicache/runtime/allocator_calls.hpp"
#include "markov/trace_graph/modules/hicache/runtime/collective_services.hpp"
#include "markov/trace_graph/modules/hicache/runtime/io_completion.hpp"
#include "markov/trace_graph/modules/hicache/runtime/layer_calls.hpp"
#include "markov/trace_graph/modules/hicache/runtime/lifecycle_calls.hpp"
#include "markov/trace_graph/modules/hicache/runtime/loads.hpp"
#include "markov/trace_graph/modules/hicache/runtime/phase_calls.hpp"
#include "markov/trace_graph/modules/hicache/runtime/prefetch_queries.hpp"
#include "markov/trace_graph/modules/hicache/runtime/prefetch_waits.hpp"
#include "markov/trace_graph/modules/hicache/runtime/storage_writes.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_calls.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_confirmations.hpp"
#include "markov/trace_graph/source_dag/index.hpp"
#include "markov/trace_graph/structure/hicache/phase.hpp"
#include "markov/trace_graph/structure/hicache/resources.hpp"
#include "markov/trace_graph/structure/missing_work.hpp"
#include "modules/hicache/runtime/source_preparation.hpp"
#include <algorithm>
#include <cstdlib>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace markov::trace_graph::modules::hicache::runtime {
namespace {

bool compact_execution_programs(const simulation::NodeCostAtStart & transform) {
    const auto * fine = std::getenv("TRACE_GRAPH_FINE_PROGRAMS");
    return !transform && (!fine || std::string_view(fine) != "1");
}

/** One target owns its mutable graph, cache state, callbacks and cost bindings.
 * Source/config/HTTP inputs are borrowed for this synchronous run. Resource
 * maps are populated before binding; the collaborators retain their maps by
 * reference, so no second resource or execution state is maintained. */
class HiCacheExecution {
public:
    HiCacheExecution(core::DagGraph & graph, const HiCacheSourcePreparation & prepared, const frontend::HiCacheConfig & config, std::string_view source_policy,
                     const core::ClientRequestChain & chain, uint64_t begin_us, uint64_t end_us, const simulation::NodeCostAtStart & final_cost_transform,
                     bool report_structure)
        : graph_(graph),
          source_graph_(prepared.index.graph()),
          prepared_(prepared),
          config_(config),
          source_policy_(source_policy),
          chain_(chain),
          begin_us_(begin_us),
          end_us_(end_us),
          final_cost_transform_(final_cost_transform),
          compact_programs_(compact_execution_programs(final_cost_transform)),
          io_completion_(runtime::io_completion_provider(config.io_cost)),
          replay_(source_graph_, config, io_completion_, config.io_cost.storage_batch_pages, config.io_cost.resource_lanes, true, {}, prepared.input),
          source_calls_(prepared.phase_calls),
          preparation_source_(prepared.allocator),
          operator_provider_(config.operator_models),
          loads_(stream_insertions_, config, compute_resources_.schedule, operator_provider_, io_completion_, compact_programs_),
          layer_calls_(stream_insertions_, operator_provider_),
          phase_calls_(layer_calls_, stream_insertions_, compute_resources_.forward, operator_provider_, compact_programs_,
                       report_structure ? &output_.retained_source_nodes : nullptr),
          allocator_calls_(stream_insertions_, operator_provider_, compact_programs_, report_structure ? &output_.retained_source_nodes : nullptr),
          confirmations_(operator_provider_),
          writes_(replay_, config, stream_insertions_, compute_resources_.schedule, operator_provider_, io_completion_, compact_programs_),
          write_confirmations_(operator_provider_, config.page_size, compute_resources_.schedule),
          storage_writes_(replay_.state(), config, io_completion_),
          layer_consumer_([this](const std::string & pid, uint64_t batch) -> const std::vector<size_t> & { return loads_.layer_consumer(pid, batch); }) {}

    HiCacheWindowResult run();

private:
    void prepare_source();
    nlohmann::json replace_source_regions();
    void admit_fact_work(const model::HiCacheReplayFact & input, const simulation::FutureDag & future);
    void bind_phase_migrations(std::map<size_t, structure::CostMigration> operations, const simulation::FutureDag & future);
    uint64_t estimate_node_cost(size_t node, uint64_t start_us, uint64_t duration);
    void expand_at_start(size_t node, uint64_t elapsed_us, simulation::FutureDag & future);
    void finish();

    core::DagGraph & graph_;
    const core::DagGraph & source_graph_;
    const HiCacheSourcePreparation & prepared_;
    const frontend::HiCacheConfig & config_;
    std::string_view source_policy_;
    const core::ClientRequestChain & chain_;
    uint64_t begin_us_, end_us_;
    simulation::NodeCostAtStart final_cost_transform_;
    bool compact_programs_;
    HiCacheWindowResult output_;
    structure::hicache::IoCompletionProvider io_completion_;
    model::HiCacheModelReplay replay_;
    const adapters::sglang::PhaseCallAudit & source_calls_;
    const adapters::sglang::AllocatorPreparationObservation & preparation_source_;
    std::map<size_t, std::vector<size_t>> facts_at_;
    HiCachePrefetchQueries queries_;
    HiCachePrefetchWaits waiting_;
    HiCacheWriteStreamInsertions stream_insertions_;
    structure::hicache::SchedulerDeviceResources compute_resources_;
    cost::OperatorCostProvider operator_provider_;
    HiCacheLoads loads_;
    HiCacheLayerCalls layer_calls_;
    HiCachePhaseCalls phase_calls_;
    HiCacheCollectiveServices collective_services_;
    HiCacheAllocatorCalls allocator_calls_;
    HiCacheQueueConfirmations confirmations_;
    HiCacheWriteCalls writes_;
    HiCacheWriteConfirmations write_confirmations_;
    HiCacheStorageWrites storage_writes_;
    HiCacheLifecycleCalls lifecycles_;
    HiCacheLayerCalls::Consumer layer_consumer_;
    std::map<size_t, uint64_t> operator_costs_;
    std::set<size_t> phase_cost_migrations_;
    size_t prepared_node_count_ = 0;
    size_t admitted_prefills_ = 0, admitted_allocations_ = 0;
};

HiCacheWindowResult HiCacheExecution::run() {
    core::ExecutionMetrics metrics;
    output_.final_cost_transform_enabled = static_cast<bool>(final_cost_transform_);
    prepare_source();
    metrics.checkpoint("source_observations_and_binding");

    auto replacement_phases = replace_source_regions();
    prepared_node_count_ = graph_.node_count();
    metrics.checkpoint("source_region_replacement");

    output_.simulation = simulation::run_topological_simulation(
        graph_,
        [this](size_t node, uint64_t start_us, uint64_t duration) { return estimate_node_cost(node, start_us, duration); },
        [this](size_t node, uint64_t elapsed_us, simulation::FutureDag & future) { expand_at_start(node, elapsed_us, future); },
        compact_programs_);
    metrics.checkpoint("execution_including_dynamic_structure_and_cost_queries");

    finish();
    metrics.checkpoint("execution_accounting");
    output_.performance = metrics.report();
    output_.performance["compact_programs"] = compact_programs_;
    output_.performance["executed_nodes"] = output_.simulation.processed_nodes;
    output_.performance["expansion_callbacks"] = output_.simulation.expansion_callbacks;
    output_.performance["source_replacement_phases"] = std::move(replacement_phases);
    output_.performance["operator_queries_nested"] = operator_provider_.performance();
    return std::move(output_);
}

void HiCacheExecution::prepare_source() {
    replay_.prepare_decode_allocations();
    output_.phase_observation_issues = source_calls_.issues();
    if (!output_.phase_observation_issues.empty())
        throw structure::MissingWork("Phase replacement needs complete per-call source observations", output_.phase_observation_issues);

    output_.prepared_facts = replay_.facts().size();
    const auto bound = bind_hicache_execution_boundaries(graph_, replay_.facts());
    if (!bound.issues.empty()) {
        auto gaps = nlohmann::json::array();
        for (const auto & [fact, issues] : bound.issues)
            gaps.push_back({
                { "source_fact",   fact },
                {      "issues", issues }
            });

        throw structure::MissingWork("HiCache state facts lack execution boundaries", std::move(gaps));
    }
    for (const auto & item : replay_.facts()) facts_at_[bound.fact_nodes.at(item.fact.source_node_id)].push_back(item.fact.source_node_id);
}

nlohmann::json HiCacheExecution::replace_source_regions() {
    core::ExecutionMetrics metrics;
    stream_insertions_.bind_synchronizations(graph_);
    compute_resources_ = adapters::sglang::observe_scheduler_compute_resources(source_dag::SourceDagIndex(graph_));
    queries_.bind(graph_, replay_, operator_provider_, facts_at_, io_completion_);
    const HiCacheQueueConfirmations::WriteWork confirmation_work = [this](const WriteConfirmationWork & work, simulation::FutureDag & future) {
        return write_confirmations_.expand(work, replay_.state(), writes_, future);
    };
    waiting_.bind(graph_, replay_, source_policy_, config_.prefetch_policy, chain_, operator_provider_, confirmation_work);
    for (const auto & [node, ids] : facts_at_)
        if (!ids.empty() && !graph_.node(node).active) throw std::runtime_error("Wait replacement removed a state fact boundary");
    metrics.checkpoint("prefetch_queries_and_waits");
    loads_.bind(graph_, replay_, begin_us_, end_us_);
    layer_calls_.bind(graph_, begin_us_, end_us_);
    lifecycles_.bind(graph_, replay_, begin_us_, end_us_, chain_.requests.front().start);
    for (auto & [node, ids] : facts_at_) std::erase_if(ids, [this](size_t id) { return lifecycles_.owns(id); });
    metrics.checkpoint("loads_layers_and_lifecycles");
    write_confirmations_.prepare(graph_, begin_us_, end_us_, waiting_.replaced_intervals(), chain_.hicache_idle_since_us);
    // Confirmation tails can synchronize source write Records even after the
    // nonblocking readiness check succeeds. Replace those consumers before
    // replacing write producers. Target acknowledgement dependencies are
    // generated separately from target completion/publication state.
    write_confirmations_.replace_source_tails(graph_);
    writes_.bind(graph_, begin_us_, end_us_, chain_.requests.front().start);
    for (const auto & [node, ids] : facts_at_)
        if (!ids.empty() && !graph_.node(node).active) throw std::runtime_error("Eviction replacement removed a state input boundary");
    metrics.checkpoint("write_submissions_and_tails");
    confirmations_.bind(graph_, replay_, begin_us_, end_us_, waiting_.replaced_intervals(), confirmation_work, chain_.hicache_idle_since_us);
    confirmations_.replace_load_tails(graph_);
    writes_.replace_active_load_branches(graph_);
    loads_.replace_source_submissions(graph_);
    metrics.checkpoint("load_submissions_and_confirmations");
    {
        const source_dag::SourceDagIndex source(graph_);
        core::DagMutationPlan plan{ .component = "cpu_collective" };
        output_.cpu_collectives = patch::append_retained_cpu_collectives(source, observe_cpu_collectives(source), plan);
        if (output_.cpu_collectives.status == "blocked") throw std::runtime_error("CPU collective composition failed");
        const auto mutation = core::apply_dag_mutation_plan(graph_, plan);
#ifdef DEBUG
        if (!mutation.topology.ok()) throw std::runtime_error("CPU collective topology invalid");
#endif
    }
    metrics.checkpoint("retained_cpu_collectives");
    collective_services_.bind(graph_, prepared_, source_calls_.collectives.calls, begin_us_, end_us_);
    metrics.checkpoint("collective_services");
    phase_calls_.bind(graph_, source_graph_, source_calls_.attention.calls, source_calls_.metadata.calls, source_calls_.dense.calls, begin_us_, end_us_);
    metrics.checkpoint("phase_call_regions");
    allocator_calls_.bind(graph_, prepared_, replay_.facts(), compute_resources_.schedule, begin_us_, end_us_);
    metrics.checkpoint("allocator_call_regions");
    // No source regions are replaced after this point. Every generated host
    // operation binds against the same surviving worker queues.
    {
        const source_dag::SourceDagIndex retained(graph_);
        const auto workers = observe_host_worker_members(graph_);
        loads_.rebind_retained_resources(retained, workers);
        layer_calls_.rebind_retained_resources(retained, workers);
        phase_calls_.rebind_retained_resources(retained, workers);
        allocator_calls_.rebind_resources(retained, workers);
        writes_.rebind_retained_resources(retained, workers);
        write_confirmations_.rebind_resources(workers);
    }
    metrics.checkpoint("retained_resource_binding");
    for (const auto & [node, ids] : facts_at_)
        if (!ids.empty()) graph_.mutable_node(node).observe_start = true;
    for (const auto & request : chain_.requests) {
        graph_.mutable_node(request.start).observe_start = true;
        graph_.mutable_node(request.completion).observe_start = true;
    }
    return metrics.phases();
}

void HiCacheExecution::admit_fact_work(const model::HiCacheReplayFact & input, const simulation::FutureDag & future) {
    ++output_.consumed_facts;
    const auto & phase_work = replay_.current_phase_work();
    if (admitted_allocations_ < phase_work.allocator_calls.size()) {
        const auto preparation = adapters::sglang::plan_allocator_work(preparation_source_.supported_pids, phase_work.allocator_calls);
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
        for (; admitted_allocations_ < phase_work.allocator_calls.size(); ++admitted_allocations_)
            allocator_calls_.set_work(phase_work.allocator_calls[admitted_allocations_], preparation.calls[admitted_allocations_]);
    }
    if (input.role == HiCacheFactRole::CacheDecodeAllocation) {
        ++output_.decode_allocations;
        return;
    }
    if (!phase_work.blockers.empty()) throw structure::MissingWork("Phase input incomplete", phase_work.blockers);

    // The source join admits one phase per request/rank. This cursor
    // consumes each appended work item once; no second identity set is needed.
    for (; admitted_prefills_ < phase_work.prefills.size(); ++admitted_prefills_) {
        const auto & prefill = phase_work.prefills[admitted_prefills_];
        const auto & decode = phase_work.decodes[admitted_prefills_];
        const auto source_phase_index = replay_.source_phase_index(prefill.pid, prefill.request_id);
        const auto & source_phase = replay_.source_phases().observations.at(source_phase_index);
        auto work = structure::hicache::plan_phase_calls(source_calls_, source_phase_index, prefill, decode);
        for (auto & call : work.tensors) {
            using Action = structure::hicache::PhaseCallPlan::Action;
            if (call.action == Action::RetainCompute) ++output_.phase_migration_candidates;
            else if (call.action == Action::Generate) ++output_.phase_generated_plans;
            else ++output_.phase_plan_issues[call.reason];

            phase_calls_.set_work(std::move(call));
        }
        for (auto & call : work.collectives) {
            output_.collective_work.push_back(std::move(call));
            collective_services_.set_work(output_.collective_work.back());
        }

        auto operations = structure::hicache::phase_migration_candidates(source_graph_, source_phase, prefill, decode);
        bind_phase_migrations(std::move(operations), future);
    }
}

void HiCacheExecution::bind_phase_migrations(std::map<size_t, structure::CostMigration> operations, const simulation::FutureDag & future) {
    std::erase_if(operations, [this](const auto & item) {
        return phase_calls_.owns(item.first) || allocator_calls_.owns(item.first) || collective_services_.owns(item.first);
    });

    structure::hicache::require_complete_phase_migrations(operations);
    const auto costs = cost::migrate_source_costs(source_graph_, operations, output_.phase_operator_costs);
    for (const auto & cost : costs) {
        phase_cost_migrations_.insert(cost.node_id);

        // Unchanged CPU work may already have executed. A no-op is
        // not a retroactive update; actual changes are.
        const bool started = !future.can_bind_cost(cost.node_id);
        const auto bound_duration = started && final_cost_transform_
                                        ? final_cost_transform_(cost.node_id, graph_.node(cost.node_id).simulation_start, cost.duration_us)
                                        : cost.duration_us;
        if (bound_duration == graph_.node(cost.node_id).duration) continue;
        if (started) throw std::runtime_error("Phase admitted after an operator already started");
        if (!operator_costs_.emplace(cost.node_id, cost.duration_us).second) throw std::runtime_error("Duplicate phase ownership");
    }
}

uint64_t HiCacheExecution::estimate_node_cost(size_t node, uint64_t start_us, uint64_t duration) {
    // Dynamic programs already carry bound costs; collective/source overrides
    // address only nodes prepared before execution. NodeScale still visits all.
    if (node >= prepared_node_count_ && !final_cost_transform_) return duration;

    if (operator_costs_.contains(node)) duration = operator_costs_.at(node);
    if (const auto cost = collective_services_.caller_cost(graph_, node, operator_provider_)) duration = *cost;
    if (const auto cost = collective_services_.worker_cost(graph_, node, operator_provider_)) duration = *cost;
    if (const auto work = collective_services_.demand(graph_, node)) {
        const auto estimate = operator_provider_.estimate(*work);
        duration = estimate.duration_us;
        auto & event = graph_.mutable_event_for_node(node);
        event.set_arg("cost_binding_source",
                      work->timing_scope == "device_service" ? "collective_event_independent_model" : "device_collective_independent_model");
    }

    return final_cost_transform_ ? final_cost_transform_(node, start_us, duration) : duration;
}

void HiCacheExecution::expand_at_start(size_t node, uint64_t elapsed_us, simulation::FutureDag & future) {
    const auto time = begin_us_ + elapsed_us;
    if (node == chain_.requests.front().start) {
        queries_.start(future);
        waiting_.start(future);
    }
    for (const auto id : lifecycles_.advance(node, time, replay_)) {
        const auto & input = replay_.fact(id);
        writes_.applied(input);
        if (input.consumed) admit_fact_work(input, future);
    }
    for (const auto id : writes_.advance(node, time, future)) admit_fact_work(replay_.fact(id), future);
    waiting_.advance(node, time, future);
    confirmations_.advance(node, time, future);
    write_confirmations_.advance(node, time, replay_.state());
    if (const auto at = facts_at_.find(node); at != facts_at_.end())
        for (const auto id : at->second) {
            const auto & input = replay_.fact(id);
            replay_.apply(id, time, node);
            writes_.applied(input);
            if (input.consumed) admit_fact_work(input, future);
        }
    loads_.advance(node, time, future);
    phase_calls_.advance(graph_, node, layer_consumer_, future);
    allocator_calls_.advance(graph_, node, future);
    layer_calls_.advance(node, layer_consumer_, future);
    queries_.advance(node, time, future);
    storage_writes_.advance(node, time, future);
}

void HiCacheExecution::finish() {
    output_.model = replay_.finish();
    output_.admission_retry_rank_calls = waiting_.admission_retry_rank_calls();
    output_.prepared_phase_calls = phase_calls_.prepared();
    output_.prepared_collective_services = collective_services_.prepared();
    output_.prepared_collective_events = collective_services_.prepared_events();
    output_.prepared_collective_worker_segments = collective_services_.prepared_workers();
    output_.prepared_collective_caller_segments = collective_services_.prepared_callers();
    output_.executed_phase_calls = phase_calls_.executed();
    if (output_.executed_phase_calls != output_.prepared_phase_calls) throw std::runtime_error("Execution did not expand every prepared attention call");
    output_.prepared_allocator_calls = allocator_calls_.prepared();
    output_.executed_allocator_calls = allocator_calls_.executed();
    output_.retained_allocator_calls = allocator_calls_.retained();
    if (output_.executed_allocator_calls != output_.prepared_allocator_calls)
        throw std::runtime_error("Execution did not expand every prepared allocator call");
    if (!output_.model.effect_decisions.missing_facts.empty()) throw std::runtime_error("HiCache execution finished with missing model facts");
    output_.phase_admissions = admitted_prefills_;
    output_.changed_operators = operator_costs_.size();
    output_.source_load_submissions = loads_.source_submissions();
    output_.load_submission_shared_cost_batches = loads_.shared_cost_batches();
    output_.completed_load_batches = loads_.completed_batches();
    output_.prepared_layer_calls = layer_calls_.prepared_calls();
    output_.active_layer_calls = layer_calls_.active_calls();
    output_.inactive_layer_calls = layer_calls_.inactive_calls();
    output_.source_write_submissions = writes_.source_submissions();
    output_.completed_writes = writes_.completed_writes();
    output_.resumed_allocations = writes_.resumed_allocations();
    output_.observed_write_fragments = writes_.observed_write_fragments();
    output_.observed_eviction_fragments = writes_.observed_eviction_fragments();
    output_.expanded_write_submissions = writes_.expanded_submissions();
    output_.generated_control_steps = writes_.generated_control_steps();
    output_.generated_blocking_checks = writes_.generated_blocking_checks();
    output_.confirmations = confirmations_.coverage();
    const auto endpoint = chain_.requests.back().completion;
    const auto formal_start = chain_.requests[chain_.formal_begin_index].start;
    output_.http_us = graph_.node(endpoint).completion_time - graph_.node(formal_start).completion_time;
    output_.independent_operator_queries = operator_provider_.queries();

    // Binding provenance is independent of whether a numerical override was
    // needed. Never infer migration merely from a surviving trace node.
    for (const auto & node : graph_.nodes()) {
        if (!node.active) continue;
        const auto id = node.id;

        std::string origin = "unclassified";
        if (phase_cost_migrations_.contains(id)) origin = "phase_source_migration";
        else if (node.kind == core::DagNodeKind::Synthetic) origin = graph_.event_for_node(id).arg("cost_binding_source", "unclassified");
        if (origin == "device_collective_independent_model") ++output_.executed_collective_services;
        if (origin == "collective_event_independent_model") ++output_.executed_collective_events;
        if (origin == "collective_worker_independent_model") ++output_.executed_collective_worker_segments;
        if (origin == "collective_caller_independent_model") ++output_.executed_collective_caller_segments;

        auto & totals = output_.executed_cost_sources[origin];
        ++totals.nodes;
        totals.node_duration_us += node.duration;
        totals.sequential_gap_us += node.cpu_gap_after;
        totals.ready_delay_us += node.cpu_ready_delay_before;
    }

#ifdef DEBUG
    // TRACE_GRAPH_DEBUG enables this independent validation replay. Normal
    // prediction uses the execution above, without simulating the DAG twice.
    std::vector<std::pair<uint64_t, uint64_t>> times;
    times.reserve(graph_.node_count());
    for (const auto & node : graph_.nodes()) times.emplace_back(node.simulation_start, node.completion_time);

    (void)simulation::run_topological_simulation(graph_);
    for (const auto & node : graph_.nodes())
        if (node.active && times[node.id] != std::pair{ node.simulation_start, node.completion_time })
            throw std::runtime_error("HiCache expanded DAG differs from its executed timing");
#endif
}

} // namespace

HiCacheWindowResult execute_hicache_window(core::DagGraph & graph, const HiCacheSourcePreparation & prepared, const frontend::HiCacheConfig & config,
                                           std::string_view source_policy, const core::ClientRequestChain & chain, uint64_t begin_us, uint64_t end_us,
                                           const simulation::NodeCostAtStart & final_cost_transform, bool report_structure) {
    if (chain.status != "connected" || chain.requests.empty() || end_us <= begin_us)
        throw std::invalid_argument("HiCache execution requires a connected serial HTTP window");

    return HiCacheExecution(graph, prepared, config, source_policy, chain, begin_us, end_us, final_cost_transform, report_structure).run();
}

} // namespace markov::trace_graph::modules::hicache::runtime
