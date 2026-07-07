/**
 * @file
 * @brief trace_graph C++ CLI 的主执行入口。
 *
 * CLI 只负责把输入 trace、model config、模块执行和 summary 输出串成稳定 workflow；
 * 建模判断保留在 DagBuilder、SimulationModule 和 diagnostics 层。
 */
#include "markov/trace_graph/cli/debug_support.hpp"
#include "markov/trace_graph/core/dag_builder.hpp"
#include "markov/trace_graph/core/logger.hpp"
#include "markov/trace_graph/frontend/model_config.hpp"
#include "markov/trace_graph/frontend/trace_normalizer.hpp"
#include "markov/trace_graph/io/chrome_trace_io.hpp"
#include "markov/trace_graph/io/trace_manifest_input.hpp"
#include "markov/trace_graph/modules/hicache/hicache_module.hpp"
#include "markov/trace_graph/modules/module.hpp"
#include "markov/trace_graph/modules/node_scale/node_scale_module.hpp"
#include "markov/trace_graph/simulation/topological_simulator.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#ifdef DEBUG
#include <chrono>
#endif
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace cli_detail {

using Json = nlohmann::json;
using DagBuilder = markov::trace_graph::core::DagBuilder;
using DagGraph = markov::trace_graph::core::DagGraph;
using Logger = markov::trace_graph::core::Logger;
using ModelConfig = markov::trace_graph::frontend::ModelConfig;
using SimulationModule = markov::trace_graph::modules::SimulationModule;

struct CliOptions {
    /**
     * manifest 输入在 C++ 内存中执行 trace channel 合流，不写大中间 trace。
     */
    std::string profile_manifest;
    std::string graph_output;
    std::string model_config_file;
#ifdef DEBUG
    std::string model_summary_file;
    std::string dag_analysis_output_dir;
#endif
    std::string run_summary_file;
    std::string scenario_name = "trace_graph";
    bool debug = false;
    bool verbose = false;
    bool explicit_help = false;
    bool show_help = false;
    size_t threads = 1;
    size_t file_threads = 1;
    double trace_merge_tolerance_us = 10'000.0;
    size_t trace_merge_search_window = 5;
    double trace_merge_margin_us = 100.0;
    std::string trace_merge_mode = "search";
    bool include_torch = true;
    bool include_ld_preload = true;
    bool include_python_probe = true;
};

void print_usage(const char * prog) {
    std::cout << "Usage: " << prog << " --profile-manifest FILE --run-summary FILE [OPTIONS]\n\n"
              << "Build a C++ TraceGraph DAG from profile traces and run topological simulation.\n\n"
              << "Options:\n"
              << "  -h, --help                      Show this help message\n"
              << "  --profile-manifest FILE         profile_manifest.json input; C++ reads torch/LD/probe traces directly\n"
              << "  --threads N                     Logical input read/build parallelism\n"
              << "  --file-threads N                Per-file Chrome trace event parse parallelism\n"
              << "  --trace-merge-tolerance-us N    LD_PRELOAD/profiler match tolerance in manifest mode\n"
              << "  --trace-merge-window N          LD_PRELOAD/profiler search window in manifest mode\n"
              << "  --trace-merge-margin-us N       Standalone custom event timestamp margin in manifest mode\n"
              << "  --trace-merge-mode MODE         Manifest merge mode; currently search\n"
              << "  --trace-channels LIST           Manifest trace channels to read: torch,ld_preload,python_probe\n"
              << "  --graph-output FILE             Optional Chrome trace DAG output\n"
              << "  --model-config FILE             Optional C++ model config for SimulationModule execution\n"
#ifdef DEBUG
              << "  --model-summary FILE            Optional model summary JSON output\n"
              << "  --dag-analysis-output-dir DIR   Optional validation DAG analysis artifact directory\n"
#endif
              << "  --run-summary FILE              Required run summary JSON output\n"
              << "  --scenario-name NAME            Scenario name in run summary\n"
              << "  -d, --debug                     Enable debug logging\n"
              << "  -v, --verbose                   Enable info logging\n";
}

bool consume_value(int & i, int argc, char ** argv, std::string & out, const std::string & option) {
    if (i + 1 >= argc) {
        std::cerr << "Error: " << option << " requires a value\n";
        return false;
    }
    out = argv[++i];
    return true;
}

bool consume_size_value(int & i, int argc, char ** argv, size_t & out, const std::string & option) {
    std::string value;
    if (!consume_value(i, argc, argv, value, option)) return false;
    try {
        out = std::stoull(value);
        return true;
    }
    catch (...) {
        std::cerr << "Error: " << option << " expects a non-negative integer\n";
        return false;
    }
}

bool consume_double_value(int & i, int argc, char ** argv, double & out, const std::string & option) {
    std::string value;
    if (!consume_value(i, argc, argv, value, option)) return false;
    try {
        out = std::stod(value);
        return true;
    }
    catch (...) {
        std::cerr << "Error: " << option << " expects a number\n";
        return false;
    }
}

std::string trim_channel_token(std::string value) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!value.empty() && is_space(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    while (!value.empty() && is_space(static_cast<unsigned char>(value.back()))) value.pop_back();
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool consume_trace_channels(int & i, int argc, char ** argv, CliOptions & opts, const std::string & option) {
    std::string value;
    if (!consume_value(i, argc, argv, value, option)) return false;

    bool include_torch = false;
    bool include_ld_preload = false;
    bool include_python_probe = false;
    size_t begin = 0;
    while (begin <= value.size()) {
        const auto end = value.find(',', begin);
        auto token = trim_channel_token(value.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
        if (!token.empty()) {
            if (token == "all") {
                include_torch = true;
                include_ld_preload = true;
                include_python_probe = true;
            }
            else if (token == "torch") include_torch = true;
            else if (token == "ld_preload" || token == "ld-preload" || token == "ld") include_ld_preload = true;
            else if (token == "python_probe" || token == "python-probe" || token == "probe") include_python_probe = true;
            else {
                std::cerr << "Error: " << option << " contains unknown channel: " << token << "\n";
                return false;
            }
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    if (!include_torch && !include_ld_preload && !include_python_probe) {
        std::cerr << "Error: " << option << " must enable at least one trace channel\n";
        return false;
    }
    opts.include_torch = include_torch;
    opts.include_ld_preload = include_ld_preload;
    opts.include_python_probe = include_python_probe;
    return true;
}

CliOptions parse_cli(int argc, char ** argv) {
    /**
     * @brief CLI 只保留当前主线需要的参数，不提供 --scale/-s 等快捷入口。
     *
     * what-if 必须通过 --model-config 进入 C++ SimulationModule。
     */
    CliOptions opts;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            opts.explicit_help = true;
            opts.show_help = true;
            return opts;
        }
        if (arg == "--profile-manifest") {
            if (!consume_value(i, argc, argv, opts.profile_manifest, arg)) opts.show_help = true;
        }
        else if (arg == "--threads") {
            if (!consume_size_value(i, argc, argv, opts.threads, arg)) opts.show_help = true;
        }
        else if (arg == "--file-threads") {
            if (!consume_size_value(i, argc, argv, opts.file_threads, arg)) opts.show_help = true;
        }
        else if (arg == "--trace-merge-tolerance-us") {
            if (!consume_double_value(i, argc, argv, opts.trace_merge_tolerance_us, arg)) opts.show_help = true;
        }
        else if (arg == "--trace-merge-window") {
            if (!consume_size_value(i, argc, argv, opts.trace_merge_search_window, arg)) opts.show_help = true;
        }
        else if (arg == "--trace-merge-margin-us") {
            if (!consume_double_value(i, argc, argv, opts.trace_merge_margin_us, arg)) opts.show_help = true;
        }
        else if (arg == "--trace-merge-mode") {
            if (!consume_value(i, argc, argv, opts.trace_merge_mode, arg)) opts.show_help = true;
        }
        else if (arg == "--trace-channels") {
            if (!consume_trace_channels(i, argc, argv, opts, arg)) opts.show_help = true;
        }
        else if (arg == "--graph-output") {
            if (!consume_value(i, argc, argv, opts.graph_output, arg)) opts.show_help = true;
        }
        else if (arg == "--model-config") {
            if (!consume_value(i, argc, argv, opts.model_config_file, arg)) opts.show_help = true;
        }
#ifdef DEBUG
        else if (arg == "--model-summary") {
            if (!consume_value(i, argc, argv, opts.model_summary_file, arg)) opts.show_help = true;
        }
        else if (arg == "--dag-analysis-output-dir") {
            if (!consume_value(i, argc, argv, opts.dag_analysis_output_dir, arg)) opts.show_help = true;
        }
#endif
        else if (arg == "--run-summary") {
            if (!consume_value(i, argc, argv, opts.run_summary_file, arg)) opts.show_help = true;
        }
        else if (arg == "--scenario-name") {
            if (!consume_value(i, argc, argv, opts.scenario_name, arg)) opts.show_help = true;
        }
        else if (arg == "-d" || arg == "--debug") opts.debug = true;
        else if (arg == "-v" || arg == "--verbose") opts.verbose = true;
        else {
            std::cerr << "Error: Unknown option: " << arg << "\n";
            opts.show_help = true;
            return opts;
        }
    }
    if (opts.profile_manifest.empty()) {
        std::cerr << "Error: --profile-manifest is required\n";
        opts.show_help = true;
    }
    if (opts.run_summary_file.empty()) {
        std::cerr << "Error: --run-summary is required\n";
        opts.show_help = true;
    }
    return opts;
}

std::vector<std::unique_ptr<SimulationModule>> build_modules(const std::string & model_config_file) {
    /**
     * @brief 按固定顺序构造 SimulationModule。
     *
     * 简单缩放先执行，HiCache 后执行。后续如果模块之间存在依赖，应在这里明确排序，
     * 而不是让 JSON 对象遍历顺序决定行为。
     */
    std::vector<std::unique_ptr<SimulationModule>> modules;
    if (model_config_file.empty()) return modules;
    modules.reserve(2);

    auto config = ModelConfig::from_file(model_config_file);
    if (config.node_scale.enabled) { modules.push_back(std::make_unique<markov::trace_graph::modules::node_scale::NodeScaleModule>(config.node_scale)); }
    if (config.hicache.enabled) { modules.push_back(std::make_unique<markov::trace_graph::modules::hicache::HiCacheModule>(config.hicache)); }
    return modules;
}

void write_json_file(const std::string & filename, const Json & value) {
    std::ofstream ofs(filename);
    if (!ofs.is_open()) { throw std::runtime_error("Failed to write JSON file: " + filename); }
    ofs << value.dump(2) << "\n";
}

Json run_summary_base(const CliOptions & opts, const DagGraph & graph) {
    /**
     * @brief run_summary 是 runner/validation 使用的辅助输出。
     *
     * 用户默认看到的 prediction.json 由 Python runner 从 simulated_e2e_ns 提取。
     */
    Json root;
    root["scenario_name"] = opts.scenario_name;
    root["profile_manifest"] = opts.profile_manifest.empty() ? nullptr : Json(opts.profile_manifest);
    root["graph_output"] = opts.graph_output;
    root["model_config"] = opts.model_config_file;
    root["parsed_record_count"] = graph.parsed_record_count();
    root["simulated_e2e_ns"] = graph.e2e_time();
    root["real_e2e_ns"] = graph.real_e2e_time();
    root["node_count"] = graph.node_count();
    root["edge_count"] = graph.edge_count();
    root["edge_counts_by_kind"] = graph.edge_counts_by_kind();
    root["threads"] = opts.threads;
    root["file_threads"] = opts.file_threads;
    root["trace_channels"] = Json::array();
    if (opts.include_torch) root["trace_channels"].push_back("torch");
    if (opts.include_ld_preload) root["trace_channels"].push_back("ld_preload");
    if (opts.include_python_probe) root["trace_channels"].push_back("python_probe");
    return root;
}

void write_run_summary(const std::string & filename, const CliOptions & opts, const DagGraph & graph) {
    Json root = run_summary_base(opts, graph);
    root["debug_artifacts_enabled"] = false;
    write_json_file(filename, root);
}

#ifdef DEBUG
void write_debug_run_summary(const std::string & filename, const CliOptions & opts, const DagGraph & graph, const Json & stage_timings) {
    Json root = run_summary_base(opts, graph);
    root["model_summary"] = opts.model_summary_file;
    root["dag_analysis_output_dir"] = opts.dag_analysis_output_dir;
    root["debug_artifacts_enabled"] = true;
    root["stage_timings_ms"] = stage_timings;
    write_json_file(filename, root);
}

uint64_t elapsed_ms(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
}

template <typename Fn>
auto timed_json(Json & timings, const char * key, Fn && fn) {
    const auto start = std::chrono::steady_clock::now();
    auto value = std::forward<Fn>(fn)();
    const auto end = std::chrono::steady_clock::now();
    timings[key] = elapsed_ms(start, end);
    return value;
}

template <typename Fn>
void timed_json_void(Json & timings, const char * key, Fn && fn) {
    const auto start = std::chrono::steady_clock::now();
    std::forward<Fn>(fn)();
    const auto end = std::chrono::steady_clock::now();
    timings[key] = elapsed_ms(start, end);
}
#endif

markov::trace_graph::io::ManifestTraceInputOptions manifest_options(const CliOptions & opts) {
    markov::trace_graph::io::ManifestTraceInputOptions options;
    options.threads = std::max<size_t>(1, opts.threads);
    options.file_threads = std::max<size_t>(1, opts.file_threads);
    options.tolerance_us = opts.trace_merge_tolerance_us;
    options.search_window = opts.trace_merge_search_window;
    options.margin_us = opts.trace_merge_margin_us;
    options.mode = opts.trace_merge_mode;
    options.include_torch = opts.include_torch;
    options.include_ld_preload = opts.include_ld_preload;
    options.include_python_probe = opts.include_python_probe;
    return options;
}

#ifdef DEBUG
Json build_timings_json(const DagBuilder::BuildTimings & timings) {
    Json root;
    root["normalize_ms"] = timings.normalize_ms;
    root["create_nodes_ms"] = timings.create_nodes_ms;
    root["correlation_ms"] = timings.correlation_ms;
    root["sequential_ms"] = timings.sequential_ms;
    root["event_wait_ms"] = timings.event_wait_ms;
    root["notify_wait_ms"] = timings.notify_wait_ms;
    root["model_execute_ms"] = timings.model_execute_ms;
    root["stream_sync_ms"] = timings.stream_sync_ms;
    root["event_sync_ms"] = timings.event_sync_ms;
    root["device_sync_ms"] = timings.device_sync_ms;
    root["finalize_ms"] = timings.finalize_ms;
    root["real_e2e_ms"] = timings.real_e2e_ms;
    return root;
}
#endif

DagGraph graph_from_events(std::vector<markov::trace_graph::core::TraceEvent> events, int input_index, size_t build_threads) {
    markov::trace_graph::frontend::normalize_trace_events(events);
    DagBuilder builder(build_threads);
    return builder.build(std::move(events), input_index);
}

#ifdef DEBUG
struct InputBuildResult {
    size_t index = 0;
    DagGraph graph;
    uint64_t elapsed_ms = 0;
    DagBuilder::BuildTimings build_timings;
};

InputBuildResult graph_from_events_with_timings(std::vector<markov::trace_graph::core::TraceEvent> events, int input_index, size_t build_threads) {
    const auto build_start = std::chrono::steady_clock::now();
    markov::trace_graph::frontend::normalize_trace_events(events);
    DagBuilder builder(build_threads);
    DagBuilder::BuildTimings build_timings;
    auto graph = builder.build_with_timings(std::move(events), input_index, build_timings);
    const auto build_end = std::chrono::steady_clock::now();
    return InputBuildResult{
        .index = static_cast<size_t>(input_index),
        .graph = std::move(graph),
        .elapsed_ms = cli_detail::elapsed_ms(build_start, build_end),
        .build_timings = build_timings,
    };
}
#endif

std::vector<DagGraph> build_graphs_from_manifest(const CliOptions & opts) {
    auto inputs = markov::trace_graph::io::load_trace_inputs_from_manifest(opts.profile_manifest, manifest_options(opts));

    std::vector<DagGraph> graphs(inputs.size());
    const size_t concurrency = std::max<size_t>(1, std::min(opts.threads, inputs.size()));
    for (size_t base = 0; base < inputs.size(); base += concurrency) {
        std::vector<std::future<std::pair<size_t, DagGraph>>> futures;
        auto end = std::min(inputs.size(), base + concurrency);
        futures.reserve(end - base);
        for (size_t i = base; i < end; ++i) {
            futures.push_back(std::async(std::launch::async, [&inputs, i, build_threads = opts.threads] {
                auto graph = graph_from_events(std::move(inputs[i].events), static_cast<int>(i), build_threads);
                return std::make_pair(i, std::move(graph));
            }));
        }
        for (auto & future : futures) {
            auto [index, graph] = future.get();
            graphs[index] = std::move(graph);
        }
    }
    return graphs;
}

#ifdef DEBUG
std::vector<DagGraph> build_graphs_from_manifest_with_timings(const CliOptions & opts, Json & timings) {
    auto inputs = timed_json(timings, "read_ms",
                             [&] { return markov::trace_graph::io::load_trace_inputs_from_manifest(opts.profile_manifest, manifest_options(opts)); });

    std::vector<DagGraph> graphs(inputs.size());
    Json build_inputs = Json::array();
    for (size_t i = 0; i < inputs.size(); ++i) build_inputs.push_back(Json::object());
    uint64_t build_worker_ms_sum = 0;

    const auto build_wall_start = std::chrono::steady_clock::now();
    const size_t concurrency = std::max<size_t>(1, std::min(opts.threads, inputs.size()));
    for (size_t base = 0; base < inputs.size(); base += concurrency) {
        std::vector<std::future<InputBuildResult>> futures;
        auto end = std::min(inputs.size(), base + concurrency);
        futures.reserve(end - base);
        for (size_t i = base; i < end; ++i) {
            futures.push_back(std::async(std::launch::async, [&inputs, i, build_threads = opts.threads] {
                return graph_from_events_with_timings(std::move(inputs[i].events), static_cast<int>(i), build_threads);
            }));
        }
        for (auto & future : futures) {
            auto result = future.get();
            graphs[result.index] = std::move(result.graph);
            build_worker_ms_sum += result.elapsed_ms;
            build_inputs[result.index] = build_timings_json(result.build_timings);
        }
    }
    const auto build_wall_end = std::chrono::steady_clock::now();
    timings["build_ms"] = elapsed_ms(build_wall_start, build_wall_end);
    timings["build_worker_ms_sum"] = build_worker_ms_sum;
    timings["build_inputs"] = std::move(build_inputs);
    return graphs;
}
#endif

DagGraph merge_graphs(std::vector<DagGraph> graphs) {
    /**
     * @brief 多输入 trace 在 per-rank base DAG 构好后 merge，只追加跨 rank 约束。
     */
    return DagGraph::merge(std::move(graphs));
}

void apply_modules(DagGraph & graph, const std::vector<std::unique_ptr<SimulationModule>> & modules, Logger & logger) {
    std::ranges::for_each(modules, [&](const auto & module) {
        logger.info() << "Applying module: " << module->name();
        module->apply(graph);
    });
    markov::trace_graph::cli::validate_modules(modules, logger);
}

void simulate_graph(DagGraph & graph) {
    /**
     * @brief 所有模块修改完成后只跑一次拓扑仿真。
     */
    (void)markov::trace_graph::simulation::run_topological_simulation(graph);
}

void maybe_write_graph_output(const CliOptions & opts, const DagGraph & graph) {
    if (!opts.graph_output.empty()) markov::trace_graph::io::write_chrome_trace_dag(opts.graph_output, graph);
}

#ifdef DEBUG
void write_debug_artifacts(const CliOptions & opts,
                           const DagGraph & graph,
                           const std::vector<std::unique_ptr<SimulationModule>> & modules,
                           Json & timings) {
    if (!opts.model_summary_file.empty()) markov::trace_graph::cli::write_module_summary(opts.model_summary_file, modules);
    if (opts.dag_analysis_output_dir.empty()) return;

    auto artifact_timings = markov::trace_graph::cli::write_dag_analysis_artifacts(opts.dag_analysis_output_dir, graph, opts.threads);
    Json timing_json = Json::object();
    for (const auto & item : artifact_timings) timing_json[item.first] = item.second;
    timings["dag_analysis_artifacts"] = std::move(timing_json);
}

void write_debug_failure_artifacts(const CliOptions & opts, const DagGraph & graph, const std::string & error_message, Logger & logger) {
    if (opts.dag_analysis_output_dir.empty()) return;
    try {
        markov::trace_graph::cli::write_dag_cycle_witness_artifact(opts.dag_analysis_output_dir, graph, error_message);
    }
    catch (const std::exception & artifact_error) {
        logger.warn() << "Failed to write DAG cycle witness artifact: " << artifact_error.what();
    }
}
#endif

int run_workflow(const CliOptions & opts, Logger & logger) {
#ifdef DEBUG
    Json timings;
    auto graphs = build_graphs_from_manifest_with_timings(opts, timings);
    auto graph = timed_json(timings, "merge_ms", [&] { return merge_graphs(std::move(graphs)); });
    auto modules = build_modules(opts.model_config_file);
    timed_json_void(timings, "module_ms", [&] { apply_modules(graph, modules, logger); });
    try {
        timed_json_void(timings, "simulation_ms", [&] { simulate_graph(graph); });
    }
    catch (const std::exception & e) {
        write_debug_failure_artifacts(opts, graph, e.what(), logger);
        throw;
    }
    maybe_write_graph_output(opts, graph);
    write_debug_artifacts(opts, graph, modules, timings);
    write_debug_run_summary(opts.run_summary_file, opts, graph, timings);
#else
    auto graphs = build_graphs_from_manifest(opts);
    auto graph = merge_graphs(std::move(graphs));
    auto modules = build_modules(opts.model_config_file);
    apply_modules(graph, modules, logger);
    simulate_graph(graph);
    maybe_write_graph_output(opts, graph);
    write_run_summary(opts.run_summary_file, opts, graph);
#endif
    return 0;
}

} // namespace cli_detail

using cli_detail::Logger;
using cli_detail::parse_cli;
using cli_detail::print_usage;
using cli_detail::run_workflow;

int main(int argc, char ** argv) {
    auto opts = parse_cli(argc, argv);
    if (opts.show_help) {
        print_usage(argv[0]);
        return opts.explicit_help ? 0 : 1;
    }

    auto & logger = Logger::instance();
    if (opts.debug) {
        logger.set_level(Logger::Debug);
        setenv("DEBUG_TRACE", "1", 1);
    }
    else if (opts.verbose) logger.set_level(Logger::Info);
    else logger.set_level(Logger::Warn);

    try {
        return run_workflow(opts, logger);
    }
    catch (const std::exception & e) {
        logger.error() << e.what();
        return 1;
    }
}
