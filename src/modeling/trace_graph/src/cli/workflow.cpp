/**
 * @file
 * @brief Orchestrates one complete C++ TraceGraph backend execution.
 *
 * Release and Debug builds share the same business sequence. Debug-only profiling
 * and artifact generation are injected at phase boundaries without duplicating the
 * workflow itself.
 */
#include "workflow.hpp"

#include "module_pipeline.hpp"
#include "options.hpp"
#include "run_summary.hpp"
#include "hicache_observations.hpp"
#include <nlohmann/json.hpp>

#include "markov/trace_graph/cli/debug_support.hpp"
#include "markov/trace_graph/core/dag_builder.hpp"
#include "markov/trace_graph/core/dag_graph.hpp"
#include "markov/trace_graph/core/logger.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/frontend/trace_normalizer.hpp"
#include "markov/trace_graph/io/chrome_trace_io.hpp"
#include "markov/trace_graph/io/trace_manifest_input.hpp"
#include "markov/trace_graph/modules/hicache/dag_patch_module.hpp"
#include "markov/trace_graph/modules/hicache/phase_observation.hpp"
#include "markov/trace_graph/simulation/topological_simulator.hpp"

#include <algorithm>
#include <future>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace markov::trace_graph::cli {

namespace {

using core::DagBuilder;
using core::DagGraph;

#ifdef DEBUG

void set_observed_e2e_time(DagGraph & graph) {
    uint64_t real_min = 0;
    uint64_t real_max = 0;
    bool has_real_time = false;
    for (const auto & node : graph.nodes()) {
        const auto & event = graph.event_for_node(node.id);
        if (!has_real_time || event.ts < real_min) real_min = event.ts;
        real_max = std::max(real_max, core::checked_add_u64(event.ts, event.dur, "trace timestamp overflow while measuring observed E2E"));
        has_real_time = true;
    }
    graph.set_real_e2e_time(has_real_time && real_max > real_min ? real_max - real_min : 0);
}

#endif

struct InputBuildResult {
    size_t index = 0;
    DagGraph graph;
};

/** @brief One independently buildable logical trace and its worker allocation. */
struct InputBuildRequest {
    io::ManifestTraceInput input;
    size_t index = 0;
    size_t thread_count = 1;
};

InputBuildResult build_input_graph(InputBuildRequest request) {
    if (request.index > static_cast<size_t>(std::numeric_limits<int>::max())) throw std::overflow_error("Logical trace input index exceeds GPU ID range");
    frontend::normalize_trace_events(request.input.events);
    DagBuilder builder(request.thread_count);
    auto graph = builder.build(std::move(request.input.events), static_cast<int>(request.index));
#ifdef DEBUG
    set_observed_e2e_time(graph);
#endif
    graph.set_input_contracts(std::move(request.input.input_contracts));
    graph.set_context_events(std::move(request.input.context_events));
    graph.set_prelude_context_events(std::move(request.input.prelude_context_events));
    graph.set_tail_context_events(std::move(request.input.tail_context_events));
    return InputBuildResult{
        .index = request.index,
        .graph = std::move(graph),
    };
}

std::vector<DagGraph> build_graphs(std::vector<io::ManifestTraceInput> inputs, size_t thread_budget) {
    const size_t concurrency = std::max<size_t>(1, std::min(thread_budget, inputs.size()));
    const size_t build_threads = std::max<size_t>(1, thread_budget / concurrency);
    std::vector<DagGraph> graphs(inputs.size());
    auto accept_result = [&](InputBuildResult result) { graphs[result.index] = std::move(result.graph); };

    if (concurrency == 1) {
        for (size_t index = 0; index < inputs.size(); ++index) {
            accept_result(build_input_graph(InputBuildRequest{
                .input = std::move(inputs[index]),
                .index = index,
                .thread_count = build_threads,
            }));
        }
    }
    else {
        for (size_t begin = 0; begin < inputs.size(); begin += concurrency) {
            const size_t end = std::min(inputs.size(), begin + concurrency);
            std::vector<std::future<InputBuildResult>> futures;
            futures.reserve(end - begin);
            for (size_t index = begin; index < end; ++index) {
                futures.push_back(std::async(std::launch::async, [&inputs, index, build_threads] {
                    return build_input_graph(InputBuildRequest{
                        .input = std::move(inputs[index]),
                        .index = index,
                        .thread_count = build_threads,
                    });
                }));
            }
            for (auto & future : futures) accept_result(future.get());
        }
    }
    return graphs;
}

void simulate(DagGraph & graph) {
    (void)simulation::run_topological_simulation(graph);
    (void)simulation::run_control_topological_simulation(graph);
    (void)simulation::run_gap_excluded_topological_simulation(graph);
}

nlohmann::json replay_client_requests(DagGraph & graph, const io::ManifestClientInput & input) {
    using Json = nlohmann::json;
    if (input.status != "ready") return {{"status", input.status}};
    const auto server_e2e = graph.e2e_time();
    const auto chain = core::connect_client_requests(graph, input.requests);
    if (chain.status != "connected") return {{"status", chain.status}};
    const auto replay = simulation::run_topological_simulation(graph);
    uint64_t completion = 0;
    Json requests = Json::array();
    for (const auto & request : chain.requests) {
        const auto end = graph.node(request.completion).completion_time;
        completion = std::max(completion, end);
        requests.push_back({{"request_id", request.request_id}, {"start_us", graph.node(request.start).completion_time}, {"completion_us", end}});
    }
    return {{"status", "connected"}, {"e2e_us", completion}, {"server_graph_e2e_us", server_e2e},
            {"peripheral_cost_source", "base_frontend_response_and_client_intervals"}, {"source_residual_waits_retained", true},
            {"cpu_task_queues", {{"queue_count", replay.cpu_queue_count}, {"task_count", replay.cpu_task_count},
                {"max_depth", replay.max_cpu_queue_depth},
                {"arrival_basis", "submission_return_upper_bound"}, {"submission_overlap_count", replay.submission_overlap_count},
                {"submission_overlap_total_us", replay.submission_overlap_total_us}, {"submission_overlap_max_us", replay.submission_overlap_max_us}}},
            {"component_metrics_before_client_chain", true}, {"requests", requests}};
}

#ifdef DEBUG
void run_post_simulation_diagnostics(DagGraph & graph, const ModulePipeline & pipeline) {
    for (const auto & module : pipeline.modules()) {
        if (auto * patch = dynamic_cast<modules::hicache::HiCacheDagPatchModule *>(module.get())) patch->run_causal_timing_audit(graph);
    }
}
#endif

void write_graph_output(const CliOptions & options, const DagGraph & graph) {
    if (!options.outputs.graph.empty()) io::write_chrome_trace_dag(options.outputs.graph, graph);
}

} // namespace

int run_workflow(const CliOptions & options, core::Logger & logger) {
#ifdef DEBUG
    auto modules = ModulePipeline::from_config(options.model_config,
                                               options.hicache_oracle_cost_replay,
                                               options.hicache_phase_oracle_cost_replay,
                                               options.hicache_canonical_observed_phase_scope);
#else
    auto modules = ModulePipeline::from_config(options.model_config);
#endif
    auto inputs = io::load_trace_inputs_from_manifest(options.profile_manifest, options.trace_input);
    const auto client_input = options.trace_input.include_python_probe ? io::load_client_requests_from_manifest(options.profile_manifest)
                                                                    : io::ManifestClientInput{"python_probe_channel_disabled", {}};
    auto graphs = build_graphs(std::move(inputs), options.trace_input.threads);
    auto graph = DagGraph::merge(std::move(graphs));
#ifdef DEBUG
    if (options.actual_e2e_us) graph.set_real_e2e_time(*options.actual_e2e_us);
#endif

    const auto source_io = hicache_io_observations(graph, modules::hicache::mark_observed_hicache_scope(graph));
    modules.apply(graph, logger);
    simulate(graph);
#ifdef DEBUG
    run_post_simulation_diagnostics(graph, modules);
#endif
    const auto client_result = replay_client_requests(graph, client_input);

    write_graph_output(options, graph);
#ifdef DEBUG
    if (!options.outputs.model_summary.empty()) write_module_summary(options.outputs.model_summary, modules.modules());
#endif
    write_run_summary(options.outputs.run_summary, graph, modules.modules(), source_io, client_result);
    return 0;
}

} // namespace markov::trace_graph::cli
