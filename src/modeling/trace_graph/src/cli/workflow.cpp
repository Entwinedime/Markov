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

#include "markov/trace_graph/cli/debug_support.hpp"
#include "markov/trace_graph/core/dag_builder.hpp"
#include "markov/trace_graph/core/dag_graph.hpp"
#include "markov/trace_graph/core/logger.hpp"
#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/frontend/trace_normalizer.hpp"
#include "markov/trace_graph/io/chrome_trace_io.hpp"
#include "markov/trace_graph/io/trace_manifest_input.hpp"
#include "markov/trace_graph/modules/hicache/dag_patch_module.hpp"
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
    auto modules = ModulePipeline::from_config(options.model_config, options.hicache_oracle_cost_replay);
#else
    auto modules = ModulePipeline::from_config(options.model_config);
#endif
    auto inputs = io::load_trace_inputs_from_manifest(options.profile_manifest, options.trace_input);
    auto graphs = build_graphs(std::move(inputs), options.trace_input.threads);
    auto graph = DagGraph::merge(std::move(graphs));
#ifdef DEBUG
    if (options.actual_e2e_us) graph.set_real_e2e_time(*options.actual_e2e_us);
#endif

    modules.apply(graph, logger);
    simulate(graph);
#ifdef DEBUG
    run_post_simulation_diagnostics(graph, modules);
#endif

    write_graph_output(options, graph);
#ifdef DEBUG
    if (!options.outputs.model_summary.empty()) write_module_summary(options.outputs.model_summary, modules.modules());
#endif
    write_run_summary(options.outputs.run_summary, graph, modules.modules());
    return 0;
}

} // namespace markov::trace_graph::cli
