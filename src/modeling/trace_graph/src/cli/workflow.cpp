/**
 * @file
 * @brief Orchestrates one complete C++ TraceGraph backend execution.
 *
 * Release and Debug builds share the same business sequence. Debug-only profiling
 * and artifact generation are injected at phase boundaries without duplicating the
 * workflow itself.
 */
#include "markov/trace_graph/cli/workflow.hpp"
#include "core/execution_metrics.hpp"

#include "markov/trace_graph/cli/cpu_service_preparation.hpp"
#include "markov/trace_graph/cli/file_output.hpp"
#include "markov/trace_graph/cli/hicache_observations.hpp"
#include "markov/trace_graph/cli/module_pipeline.hpp"
#include "markov/trace_graph/cli/options.hpp"
#include "markov/trace_graph/cli/run_summary.hpp"
#include "markov/trace_graph/reporting/hicache_structure.hpp"
#include "markov/trace_graph/source_dag/build.hpp"
#include "markov/trace_graph/source_dag/observations.hpp"
#include "markov/trace_graph/source_dag/replay.hpp"
#include "markov/trace_graph/structure/missing_work.hpp"
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
#include "markov/trace_graph/adapters/frameworks/sglang/hicache_observations.hpp"
#include "markov/trace_graph/cost/missing_evidence.hpp"
#include "markov/trace_graph/modules/hicache/phase_observation.hpp"
#include "markov/trace_graph/modules/hicache/runtime/control_calibration.hpp"
#include "markov/trace_graph/modules/hicache/scope_observation.hpp"
#include "markov/trace_graph/simulation/topological_simulator.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

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
    const auto formal_start = graph.node(chain.requests[chain.formal_begin_index].start).completion_time;
    Json requests = Json::array();
    for (size_t i = chain.formal_begin_index; i < chain.requests.size(); ++i) {
        const auto & request = chain.requests[i];
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
        {                                "e2e_us",                     completion - formal_start },
        {                       "formal_start_us",                                  formal_start },
        {             "preparation_request_count",                      chain.formal_begin_index },
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

PreparedSource prepare_source(const CliOptions & options, bool include_preparation) {
    core::ExecutionMetrics metrics;
    const auto client_input = options.trace_input.include_python_probe && (!options.source_observations_only || include_preparation)
                                  ? io::load_client_requests_from_manifest(options.profile_manifest, include_preparation)
                                  : io::ManifestClientInput{ "python_probe_channel_disabled", {} };
    metrics.checkpoint("client_input");

    auto trace_input = options.trace_input;
    if (client_input.formal_begin_index) trace_input.window_start_us = client_input.requests.front().start_us;

    auto inputs = io::load_trace_inputs_from_manifest(options.profile_manifest, trace_input);
    const auto source_input_selection = options.outputs.source_replay_report.empty() ? nlohmann::json(nullptr) : source_dag::input_selection_report(inputs);
    metrics.checkpoint("trace_read_parse_and_join");

    auto graph = source_dag::build(std::move(inputs), trace_input.threads);
    metrics.checkpoint("source_dag_build_and_merge");

    const auto source_execution_event_count = graph.events().size();
    // Request availability belongs to the observed source as well as target
    // execution. Bind once; the experimental replay must precede cost views
    // and any mutation, including optional NodeScale.
    const auto client_chain =
        client_input.status == "ready" && !options.source_observations_only
            ? core::connect_client_requests(graph, client_input.requests, client_input.hicache_idle_since_us, client_input.formal_begin_index)
            : core::ClientRequestChain{ client_input.status, {} };
    metrics.checkpoint("client_chain_binding");

    nlohmann::json replay;
    if (!options.outputs.source_replay_report.empty()) {
        replay = source_dag::replay_report(graph, client_chain, client_input.requests, source_execution_event_count, options.profile_manifest);
        replay["event_coverage"]["input_selection"] = source_input_selection;
    }
    metrics.checkpoint("optional_source_replay");

    if (!options.cpu_service_cost.empty()) {
        std::ifstream input(options.cpu_service_cost);
        if (!input) throw std::runtime_error("Cannot open CPU service cost file: " + options.cpu_service_cost);
        graph.cpu_service_cost() = io::read_cpu_service_cost(input, options.profile_manifest);
    }
#ifdef DEBUG
    if (options.actual_e2e_us) graph.set_real_e2e_time(*options.actual_e2e_us);
#endif

    metrics.checkpoint("cpu_correction_load");
    auto performance = metrics.report();
    performance["stored_nodes"] = graph.node_count();
    performance["stored_edges"] = graph.edge_count();
    return { std::move(graph), client_input, client_chain, std::move(replay), {}, std::move(performance) };
}

int run_prepared_workflow(const CliOptions & options, ModulePipeline & modules, PreparedSource & source, core::Logger & logger) {
    core::ExecutionMetrics metrics;
    auto & graph = source.graph;
    const auto & client_input = source.client_input;
    const auto & client_chain = source.client_chain;
    const nlohmann::json request_context = client_input.requests.empty()
                                              ? nlohmann::json{ { "status", client_input.status } }
                                              : nlohmann::json{ { "preparation_request_count", client_input.formal_begin_index },
                                                                { "formal_request_count", client_input.requests.size() - client_input.formal_begin_index },
                                                                { "observed_request_start_us", client_input.requests.front().start_us },
                                                                { "scope", "Structure includes the declared preparation prefix; HTTP measurement excludes it." } };
    if (!options.outputs.source_replay_report.empty()) write_json_file(options.outputs.source_replay_report, source.replay_report);

    if (options.source_observations_only) {
        const auto phases = modules::hicache::observe_hicache_phases(graph);
        if (!options.outputs.structure_report.empty()) {
            // The evaluator asks for structural evidence, not training data.
            // Cost extraction has additional attribution requirements and
            // must not gate comparison of independently observable structure.
            auto structure = reporting::observed_phase_structure_report(graph, phases);
            structure["request_context"] = request_context;
            write_json_file(options.outputs.structure_report, structure);
            write_json_file(options.outputs.run_summary,
                            {
                                {         "observation_scope",                        "structure" },
                                { "source_phase_observations", hicache_phase_observations(phases) }
            });
            return 0;
        }

        const adapters::sglang::HiCacheSourceIndex source(graph);
        const auto operations = modules::hicache::patch::build_hicache_io_operation_ledger(source);
        auto io = hicache_io_observations(graph, operations, !options.cpu_service_cost.empty());
        auto operator_observations = source_dag::phase_operator_observations(graph, phases, !options.cpu_service_cost.empty());
        if (!options.cpu_service_cost.empty()) {
            // Submission attribution splits a working graph at control points.
            // Keep the original observations intact and use the same paired
            // measurements as the independent write exporter. Other control
            // families are already collected by phase_operator_observations.
            auto write_graph = graph;
            auto write = modules::hicache::runtime::export_write_calibration(write_graph,
                                                                             options.trace_input.window_start_us.value_or(0),
                                                                             options.trace_input.window_end_us.value_or(std::numeric_limits<uint64_t>::max()));
            for (auto & observation : write["operator_observations"]) {
                if (observation["operation"].get_ref<const std::string &>().starts_with("write.submission."))
                    operator_observations.push_back(std::move(observation));
            }
        }

        write_json_file(options.outputs.run_summary,
                        {
                            {      "observation_scope",                           "cost" },
                            { "source_io_observations",                               io },
                            {  "operator_observations", std::move(operator_observations) }
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
    metrics.checkpoint("optional_source_observations");
    // Request availability is an input dependency, not a scoring-only adjustment.
    // Keep the same anchors throughout modeling, execution and HTTP reporting.
    const auto begin = client_input.status == "ready" ? client_input.requests.front().start_us : 0;
    const auto end = client_input.status == "ready" ? client_input.requests.back().end_us : 0;
    // A transformation gets one independent working graph. Preserve source
    // topology, observation coordinates and measured costs for migration; no
    // target module can overwrite them. Shared trace buffers remain shared.
    // Plain build/replay has no target mutation and needs no duplicate graph.
    // Keep both large graphs with the caller: fork workers reclaim them at _exit
    // without dirtying inherited allocator pages just to free individual events.
    auto & source_graph = source.migration_graph;
    if (modules.uses_hicache_execution() || !modules.modules().empty()) source_graph = std::make_unique<const DagGraph>(graph);
    metrics.checkpoint("migration_graph_copy");
    try {
        modules.apply(graph, source_graph ? *source_graph : graph, logger, client_chain, begin, end);
    }
    catch (const structure::MissingWork & error) {
        if (!options.outputs.run_summary.empty())
            write_json_file(options.outputs.run_summary,
                            {
                                {                "status",                                                                  "structure_limitation" },
                                {        "structure_gaps", nlohmann::json::array({ { { "reason", error.what() }, { "details", error.details } } }) },
                                { "requirements_complete",                                                                                   false }
            });
        throw;
    }
    catch (const cost::MissingCostEvidence & error) {
        if (!options.outputs.run_summary.empty())
            write_json_file(options.outputs.run_summary,
                            {
                                {                "status",  "data_limitation" },
                                {         "missing_costs", error.requirements },
                                { "requirements_complete",              false }
            });
        throw;
    }
    metrics.checkpoint("modules_including_execution");
    // HiCache has already executed the target DAG. Its component ownership is
    // not available, so stripped-scope replays cannot supply valid metrics.
    const auto * execution = modules.execution_result();
    const auto simulation = execution ? execution->simulation : simulation::run_topological_simulation(graph);
    if (!execution && include_source_observations) {
        (void)simulation::run_control_topological_simulation(graph);
        (void)simulation::run_gap_excluded_topological_simulation(graph);
    }
    const auto client_result = client_request_result(graph, client_chain, simulation);

    metrics.checkpoint("client_results_and_optional_replays");
    write_graph_output(options, graph);
#ifdef DEBUG
    if (!options.outputs.model_summary.empty()) write_module_summary(options.outputs.model_summary, modules.modules());
#endif
    metrics.checkpoint("optional_graph_output");
    auto performance = metrics.report();
    performance["source_preparation"] = source.performance;
    if (execution) performance["hicache_runtime_nested"] = execution->performance;
    write_run_summary(options.outputs.run_summary,
                      graph,
                      modules.modules(),
                      source_io,
                      source_phase,
                      client_result,
                      execution,
                      include_source_observations,
                      performance);
    if (!options.outputs.structure_report.empty()) {
        if (!execution) throw std::invalid_argument("Executed structure report requires HiCache execution prediction");
        auto structure =
            reporting::execution_structure_report(graph, execution->model.effect_decisions, execution->model.phase_work, execution->collective_work);
        structure["request_context"] = request_context;
        write_json_file(options.outputs.structure_report, structure);
    }
    return 0;
}

int run_workflow(const CliOptions & options, core::Logger & logger) {
    if (!options.target_plan.empty()) return run_prediction_batch(options, logger);

#ifdef DEBUG
    auto modules = ModulePipeline::from_config(options.model_config,
                                               options.hicache_oracle_cost_replay,
                                               options.hicache_phase_oracle_cost_replay,
                                               options.hicache_canonical_observed_phase_scope,
                                               options.hicache_static_replay);
#else
    auto modules = ModulePipeline::from_config(options.model_config);
#endif
    const bool include_preparation = modules.uses_hicache_execution() || (options.source_observations_only && !options.outputs.structure_report.empty());
    auto source = prepare_source(options, include_preparation);
    if (!options.prepare_cpu_service.empty()) {
        if (source.client_input.status != "ready") throw std::runtime_error("CPU service preparation needs complete client requests");
        if (source.client_chain.status != "connected") throw std::runtime_error("CPU service preparation could not bind the client chain");
        prepare_cpu_service(source.graph, options.profile_manifest, options.prepare_cpu_service, options.cpu_service_output);
        return 0;
    }

    return run_prepared_workflow(options, modules, source, logger);
}

} // namespace markov::trace_graph::cli
