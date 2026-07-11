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
#include "markov/trace_graph/simulation/topological_simulator.hpp"

#include <algorithm>
#ifdef DEBUG
#include <chrono>
#include <nlohmann/json.hpp>
#endif
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

using Json = nlohmann::json;

uint64_t elapsed_ms(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
}

Json build_timings_json(const DagBuilder::BuildTimings & timings) {
    return Json{
        {     "normalize_ms",     timings.normalize_ms },
        {  "create_nodes_ms",  timings.create_nodes_ms },
        {   "correlation_ms",   timings.correlation_ms },
        {    "sequential_ms",    timings.sequential_ms },
        {    "event_wait_ms",    timings.event_wait_ms },
        {   "notify_wait_ms",   timings.notify_wait_ms },
        { "model_execute_ms", timings.model_execute_ms },
        {   "stream_sync_ms",   timings.stream_sync_ms },
        {    "event_sync_ms",    timings.event_sync_ms },
        {   "device_sync_ms",   timings.device_sync_ms },
        {      "finalize_ms",      timings.finalize_ms },
        {      "real_e2e_ms",      timings.real_e2e_ms },
    };
}

class WorkflowDiagnostics {
public:
    template <typename Function> auto measure(const char * name, Function && function) {
        const auto start = std::chrono::steady_clock::now();
        auto value = std::forward<Function>(function)();
        timings_[name] = elapsed_ms(start, std::chrono::steady_clock::now());
        return value;
    }

    template <typename Function> void measure_void(const char * name, Function && function) {
        const auto start = std::chrono::steady_clock::now();
        std::forward<Function>(function)();
        timings_[name] = elapsed_ms(start, std::chrono::steady_clock::now());
    }

    void begin_graph_build(size_t input_count) {
        build_started_at_ = std::chrono::steady_clock::now();
        build_inputs_ = Json::array();
        for (size_t index = 0; index < input_count; ++index) build_inputs_.push_back(Json::object());
    }

    void record_graph_build(size_t index, uint64_t worker_ms, const DagBuilder::BuildTimings & timings) {
        build_worker_ms_sum_ = core::checked_add_u64(build_worker_ms_sum_, worker_ms, "DAG build worker time exceeds uint64 range");
        build_inputs_[index] = build_timings_json(timings);
    }

    void finish_graph_build() {
        timings_["build_ms"] = elapsed_ms(build_started_at_, std::chrono::steady_clock::now());
        timings_["build_worker_ms_sum"] = build_worker_ms_sum_;
        timings_["build_inputs"] = std::move(build_inputs_);
    }

    void set(const char * name, Json value) { timings_[name] = std::move(value); }
    [[nodiscard]] const Json & timings() const { return timings_; }

private:
    Json timings_ = Json::object();
    Json build_inputs_ = Json::array();
    std::chrono::steady_clock::time_point build_started_at_{};
    uint64_t build_worker_ms_sum_ = 0;
};

#else

class WorkflowDiagnostics {};

#endif

struct InputBuildResult {
    size_t index = 0;
    DagGraph graph;
#ifdef DEBUG
    uint64_t worker_ms = 0;
    DagBuilder::BuildTimings timings;
#endif
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
#ifdef DEBUG
    const auto start = std::chrono::steady_clock::now();
    DagBuilder::BuildTimings timings;
    auto graph = builder.build_with_timings(std::move(request.input.events), static_cast<int>(request.index), timings);
    return InputBuildResult{
        .index = request.index,
        .graph = std::move(graph),
        .worker_ms = elapsed_ms(start, std::chrono::steady_clock::now()),
        .timings = timings,
    };
#else
    return InputBuildResult{
        .index = request.index,
        .graph = builder.build(std::move(request.input.events), static_cast<int>(request.index)),
    };
#endif
}

std::vector<io::ManifestTraceInput> load_inputs(const CliOptions & options, WorkflowDiagnostics & diagnostics) {
#ifdef DEBUG
    return diagnostics.measure("read_ms", [&] { return io::load_trace_inputs_from_manifest(options.profile_manifest, options.trace_input); });
#else
    (void)diagnostics;
    return io::load_trace_inputs_from_manifest(options.profile_manifest, options.trace_input);
#endif
}

std::vector<DagGraph> build_graphs(std::vector<io::ManifestTraceInput> inputs, size_t thread_budget, WorkflowDiagnostics & diagnostics) {
    const size_t concurrency = std::max<size_t>(1, std::min(thread_budget, inputs.size()));
    const size_t build_threads = std::max<size_t>(1, thread_budget / concurrency);
    std::vector<DagGraph> graphs(inputs.size());
#ifdef DEBUG
    diagnostics.begin_graph_build(inputs.size());
#else
    (void)diagnostics;
#endif

    auto accept_result = [&](InputBuildResult result) {
        graphs[result.index] = std::move(result.graph);
#ifdef DEBUG
        diagnostics.record_graph_build(result.index, result.worker_ms, result.timings);
#endif
    };

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

#ifdef DEBUG
    diagnostics.finish_graph_build();
#endif
    return graphs;
}

DagGraph merge_graphs(std::vector<DagGraph> graphs, WorkflowDiagnostics & diagnostics) {
#ifdef DEBUG
    return diagnostics.measure("merge_ms", [&] { return DagGraph::merge(std::move(graphs)); });
#else
    (void)diagnostics;
    return DagGraph::merge(std::move(graphs));
#endif
}

void apply_modules(DagGraph & graph, const ModulePipeline & pipeline, core::Logger & logger, WorkflowDiagnostics & diagnostics) {
#ifdef DEBUG
    diagnostics.measure_void("module_ms", [&] { pipeline.apply(graph, logger); });
#else
    (void)diagnostics;
    pipeline.apply(graph, logger);
#endif
}

void simulate(DagGraph & graph, WorkflowDiagnostics & diagnostics) {
#ifdef DEBUG
    diagnostics.measure_void("simulation_ms", [&] { (void)simulation::run_topological_simulation(graph); });
#else
    (void)diagnostics;
    (void)simulation::run_topological_simulation(graph);
#endif
}

void write_graph_output(const CliOptions & options, const DagGraph & graph) {
    if (!options.outputs.graph.empty()) io::write_chrome_trace_dag(options.outputs.graph, graph);
}

#ifdef DEBUG
void write_debug_outputs(const CliOptions & options, const DagGraph & graph, const ModulePipeline & pipeline, WorkflowDiagnostics & diagnostics) {
    if (!options.outputs.model_summary.empty()) write_module_summary(options.outputs.model_summary, pipeline.modules());
    if (options.outputs.dag_analysis_directory.empty()) return;

    const auto artifact_timings = write_dag_analysis_artifacts(options.outputs.dag_analysis_directory, graph, options.trace_input.threads);
    Json timing_json = Json::object();
    for (const auto & [name, value] : artifact_timings) timing_json[name] = value;
    diagnostics.set("dag_analysis_artifacts", std::move(timing_json));
}
#endif

} // namespace

int run_workflow(const CliOptions & options, core::Logger & logger) {
    WorkflowDiagnostics diagnostics;
    auto modules = ModulePipeline::from_config(options.model_config);
    auto inputs = load_inputs(options, diagnostics);
    auto graphs = build_graphs(std::move(inputs), options.trace_input.threads, diagnostics);
    auto graph = merge_graphs(std::move(graphs), diagnostics);

    try {
        apply_modules(graph, modules, logger, diagnostics);
        simulate(graph, diagnostics);
    }
    catch (const std::exception & error) {
#ifdef DEBUG
        if (!options.outputs.dag_analysis_directory.empty()) { write_dag_failure_artifact(options.outputs.dag_analysis_directory, graph, error, logger); }
#endif
        throw;
    }

    write_graph_output(options, graph);
#ifdef DEBUG
    write_debug_outputs(options, graph, modules, diagnostics);
    write_run_summary(options.outputs.run_summary, graph, modules.modules(), diagnostics.timings());
#else
    write_run_summary(options.outputs.run_summary, graph, modules.modules());
#endif
    return 0;
}

} // namespace markov::trace_graph::cli
