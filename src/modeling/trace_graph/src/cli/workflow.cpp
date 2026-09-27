/**
 * @file
 * @brief Orchestrates one complete C++ TraceGraph backend execution.
 *
 * Release and Debug builds share the same business sequence. Debug-only profiling
 * and artifact generation are injected at phase boundaries without duplicating the
 * workflow itself.
 */
#include "markov/trace_graph/cli/workflow.hpp"

#include "markov/trace_graph/cli/cpu_service_preparation.hpp"
#include "markov/trace_graph/cli/file_output.hpp"
#include "markov/trace_graph/cli/hicache_observations.hpp"
#include "markov/trace_graph/cli/input_graph.hpp"
#include "markov/trace_graph/cli/module_pipeline.hpp"
#include "markov/trace_graph/cli/options.hpp"
#include "markov/trace_graph/cli/run_summary.hpp"
#include <nlohmann/json.hpp>

#include "markov/trace_graph/cli/debug_support.hpp"
#include "markov/trace_graph/core/dag_graph.hpp"
#include "markov/trace_graph/core/logger.hpp"
#include "markov/trace_graph/io/chrome_trace_io.hpp"
#include "markov/trace_graph/io/cpu_service_input.hpp"
#include "markov/trace_graph/io/trace_manifest_input.hpp"
#ifdef DEBUG
#include "markov/trace_graph/modules/hicache/dag_patch_module.hpp"
#endif
#include "markov/trace_graph/modules/hicache/missing_cost.hpp"
#include "markov/trace_graph/modules/hicache/patch/source_dag_index.hpp"
#include "markov/trace_graph/modules/hicache/phase_observation.hpp"
#include "markov/trace_graph/modules/hicache/scope_observation.hpp"
#include "markov/trace_graph/simulation/topological_simulator.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace markov::trace_graph::cli {

namespace {

using core::DagGraph;

nlohmann::json client_request_result(const DagGraph & graph, const core::ClientRequestChain & chain, const simulation::SimulationResult & replay) {
    using Json = nlohmann::json;
    if (chain.status != "connected")
        return {
            { "status", chain.status }
        };

    uint64_t completion = 0;
    Json requests = Json::array();
    for (const auto & request : chain.requests) {
        const auto end = graph.node(request.completion).completion_time;
        completion = std::max(completion, end);
        requests.push_back({
            {    "request_id",                        request.request_id },
            {      "start_us", graph.node(request.start).completion_time },
            { "completion_us",                                       end }
        });
    }
    return {
        {                                "status","connected"                                                  },
        {                                "e2e_us",                                    completion },
        {                     "full_graph_e2e_us",                                 replay.e2e_us },
        {                "peripheral_cost_source", "base_frontend_response_and_client_intervals" },
        {        "source_residual_waits_retained",                                          true },
        {                       "cpu_task_queues",
         { { "queue_count", replay.cpu_queue_count },
         { "task_count", replay.cpu_task_count },
         { "max_depth", replay.max_cpu_queue_depth },
         { "arrival_basis", "submission_return_upper_bound" },
         { "submission_overlap_count", replay.submission_overlap_count },
         { "submission_overlap_total_us", replay.submission_overlap_total_us },
         { "submission_overlap_max_us", replay.submission_overlap_max_us } }                    },
        { "component_metrics_before_client_chain",                                         false },
        {                              "requests",                                      requests }
    };
}


void write_graph_output(const CliOptions & options, const DagGraph & graph) {
    if (!options.outputs.graph.empty()) io::write_chrome_trace_dag(options.outputs.graph, graph);
}

} // namespace

int run_workflow(const CliOptions & options, core::Logger & logger) {
#ifdef DEBUG
    auto modules = ModulePipeline::from_config(options.model_config,
                                               options.hicache_oracle_cost_replay,
                                               options.hicache_phase_oracle_cost_replay,
                                               options.hicache_canonical_observed_phase_scope,
                                               options.hicache_static_replay);
#else
    auto modules = ModulePipeline::from_config(options.model_config);
#endif
    auto inputs = io::load_trace_inputs_from_manifest(options.profile_manifest, options.trace_input);
    const auto client_input = options.trace_input.include_python_probe && !options.source_observations_only
                                  ? io::load_client_requests_from_manifest(options.profile_manifest)
                                  : io::ManifestClientInput{ "python_probe_channel_disabled", {} };
    auto graph = build_input_graph(std::move(inputs), options.trace_input.threads);
    if (!options.prepare_cpu_service.empty()) {
        if (client_input.status != "ready") throw std::runtime_error("CPU service preparation needs complete client requests");
        const auto chain = core::connect_client_requests(graph, client_input.requests, client_input.hicache_idle_since_us);
        if (chain.status != "connected") throw std::runtime_error("CPU service preparation could not bind the client chain");
        prepare_cpu_service(graph, options.profile_manifest, options.prepare_cpu_service, options.cpu_service_output);
        return 0;
    }
    if (!options.cpu_service_cost.empty()) {
        std::ifstream input(options.cpu_service_cost);
        if (!input) throw std::runtime_error("Cannot open CPU service cost file: " + options.cpu_service_cost);
        graph.cpu_service_cost() = io::read_cpu_service_cost(input, options.profile_manifest);
    }
#ifdef DEBUG
    if (options.actual_e2e_us) graph.set_real_e2e_time(*options.actual_e2e_us);
#endif

    if (options.source_observations_only) {
        const modules::hicache::patch::HiCacheSourceDagIndex source(graph);
        const auto operations = modules::hicache::patch::build_hicache_io_operation_ledger(source);
        auto io = hicache_io_observations(graph, operations, !options.cpu_service_cost.empty());
        write_json_file(options.outputs.run_summary,
                        {
                            {    "source_io_observations",                                                                          io },
                            { "source_phase_observations", hicache_phase_observations(modules::hicache::observe_hicache_phases(graph)) }
        });
        return 0;
    }

    bool include_source_observations = modules.needs_hicache_observations();
#ifdef DEBUG
    include_source_observations |= !options.outputs.model_summary.empty();
#endif
    nlohmann::json source_io;
    modules::hicache::HiCachePhaseObservationAudit source_phase;
    if (include_source_observations) {
        auto source_scope = modules::hicache::observe_hicache_scope(graph);
        source_io = hicache_io_observations(graph, source_scope.operations, !options.cpu_service_cost.empty());
        // Observation alone must not lock CPU gaps before execution binds.
        if (!modules.uses_hicache_execution()) modules::hicache::apply_observed_hicache_scope(graph, source_scope);
        source_phase = std::move(source_scope.phases);
    }
    // Request availability is an input dependency, not a scoring-only adjustment.
    // Keep the same anchors throughout modeling, execution and HTTP reporting.
    const auto client_chain = client_input.status == "ready" ? core::connect_client_requests(graph, client_input.requests, client_input.hicache_idle_since_us)
                                                             : core::ClientRequestChain{ client_input.status, {} };
    const auto begin = client_input.requests.empty() ? 0 : client_input.requests.front().start_us;
    const auto end = client_input.requests.empty() ? 0 : client_input.requests.back().end_us;
    try {
        modules.apply(graph, logger, client_chain, begin, end);
    }
    catch (const modules::hicache::MissingCostEvidence & error) {
        if (!options.outputs.run_summary.empty())
            write_json_file(options.outputs.run_summary,
                            {
                                {                "status",                            "data_limitation" },
                                {         "missing_costs", nlohmann::json::array({ error.requirement }) },
                                { "requirements_complete",                                        false }
            });
        throw;
    }
    // HiCache has already executed the target DAG. Its component ownership is
    // not available, so stripped-scope replays cannot supply valid metrics.
    const auto * execution = modules.execution_result();
    const auto simulation = execution ? execution->simulation : simulation::run_topological_simulation(graph);
    if (!execution && include_source_observations) {
        (void)simulation::run_control_topological_simulation(graph);
        (void)simulation::run_gap_excluded_topological_simulation(graph);
    }
    const auto client_result = client_request_result(graph, client_chain, simulation);

    write_graph_output(options, graph);
#ifdef DEBUG
    if (!options.outputs.model_summary.empty()) write_module_summary(options.outputs.model_summary, modules.modules());
#endif
    write_run_summary(options.outputs.run_summary, graph, modules.modules(), source_io, source_phase, client_result, execution, include_source_observations);
    return 0;
}

} // namespace markov::trace_graph::cli
