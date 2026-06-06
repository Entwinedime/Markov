#include "trace_graph/core/dag_builder.hpp"
#include "trace_graph/frontend/model_config.hpp"
#include "trace_graph/frontend/trace_normalizer.hpp"
#include "trace_graph/io/chrome_trace_io.hpp"
#include "trace_graph/core/logger.hpp"
#include "trace_graph/modules/hicache/hicache_module.hpp"
#include "trace_graph/modules/node_scale_module.hpp"
#include "trace_graph/modules/simulation_module.hpp"
#include "trace_graph/simulation/topological_simulator.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Json = nlohmann::json;

struct CliOptions {
    // C++ 后端输入必须是已经由 trace_merger 合并后的 Chrome trace。
    // 多个 --input 表示多个 rank / device graph，后续由 DagGraph::merge 合并。
    std::vector<std::string> input_traces;
    std::string graph_output;
    std::string model_config_file;
    std::string model_summary_file;
    std::string run_summary_file;
    std::string scenario_name = "trace_graph";
    bool full_output = false;
    bool debug = false;
    bool verbose = false;
    bool explicit_help = false;
    bool show_help = false;
};

void print_usage(const char * prog) {
    std::cout << "Usage: " << prog << " --input TRACE [--input TRACE ...] --run-summary FILE [OPTIONS]\n\n"
              << "Build a C++ TraceGraph DAG from merged Chrome trace JSON and run topological simulation.\n\n"
              << "Options:\n"
              << "  -h, --help                  Show this help message\n"
              << "  --input FILE                Merged Chrome trace JSON input, repeatable\n"
              << "  --graph-output FILE         Optional Chrome trace DAG output\n"
              << "  --full-output               Include simulated nodes and edge flow events in --graph-output\n"
              << "  --model-config FILE         Optional C++ model config for SimulationModule execution\n"
              << "  --model-summary FILE        Optional model summary JSON output\n"
              << "  --run-summary FILE          Required run summary JSON output\n"
              << "  --scenario-name NAME        Scenario name in run summary\n"
              << "  -d, --debug                 Enable debug logging\n"
              << "  -v, --verbose               Enable info logging\n";
}

bool consume_value(int & i, int argc, char ** argv, std::string & out, const std::string & option) {
    if (i + 1 >= argc) {
        std::cerr << "Error: " << option << " requires a value\n";
        return false;
    }
    out = argv[++i];
    return true;
}

CliOptions parse_cli(int argc, char ** argv) {
    // CLI 只保留当前主线需要的参数，不再提供旧版 --scale/-s 等快捷入口。
    // what-if 必须通过 --model-config 进入 C++ SimulationModule。
    CliOptions opts;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            opts.explicit_help = true;
            opts.show_help = true;
            return opts;
        }
        if (arg == "--input") {
            std::string value;
            if (!consume_value(i, argc, argv, value, arg)) {
                opts.show_help = true;
                return opts;
            }
            opts.input_traces.push_back(value);
        }
        else if (arg == "--graph-output") {
            if (!consume_value(i, argc, argv, opts.graph_output, arg)) opts.show_help = true;
        }
        else if (arg == "--model-config") {
            if (!consume_value(i, argc, argv, opts.model_config_file, arg)) opts.show_help = true;
        }
        else if (arg == "--model-summary") {
            if (!consume_value(i, argc, argv, opts.model_summary_file, arg)) opts.show_help = true;
        }
        else if (arg == "--run-summary") {
            if (!consume_value(i, argc, argv, opts.run_summary_file, arg)) opts.show_help = true;
        }
        else if (arg == "--scenario-name") {
            if (!consume_value(i, argc, argv, opts.scenario_name, arg)) opts.show_help = true;
        }
        else if (arg == "--full-output") opts.full_output = true;
        else if (arg == "-d" || arg == "--debug") opts.debug = true;
        else if (arg == "-v" || arg == "--verbose") opts.verbose = true;
        else {
            std::cerr << "Error: Unknown option: " << arg << "\n";
            opts.show_help = true;
            return opts;
        }
    }
    if (opts.input_traces.empty()) {
        std::cerr << "Error: at least one --input is required\n";
        opts.show_help = true;
    }
    if (opts.run_summary_file.empty()) {
        std::cerr << "Error: --run-summary is required\n";
        opts.show_help = true;
    }
    return opts;
}

std::vector<std::unique_ptr<TraceGraph::SimulationModule>> build_modules(const std::string & model_config_file) {
    // 模块顺序在这里固定：简单缩放先执行，HiCache 后执行。
    // 后续如果模块之间存在依赖，应在这里明确排序，而不是让 JSON 对象遍历顺序决定行为。
    std::vector<std::unique_ptr<TraceGraph::SimulationModule>> modules;
    if (model_config_file.empty()) return modules;

    auto config = TraceGraph::ModelConfig::from_file(model_config_file);
    if (config.node_scale.enabled) modules.push_back(std::make_unique<TraceGraph::NodeScaleModule>(config.node_scale));
    if (config.hicache.enabled) modules.push_back(std::make_unique<TraceGraph::HiCacheModule>(config.hicache));
    return modules;
}

void write_json_file(const std::string & filename, const Json & value) {
    std::ofstream ofs(filename);
    if (!ofs.is_open()) { throw std::runtime_error("Failed to write JSON file: " + filename); }
    ofs << value.dump(2) << "\n";
}

void write_module_summary(const std::string & filename, const std::vector<std::unique_ptr<TraceGraph::SimulationModule>> & modules) {
    // summary 只收集已经 apply 且声明 has_summary 的模块。
    // 默认预测输出不读取 summary，避免 debug 信息参与功能路径。
    Json root;
    root["modules"] = Json::array();
    for (const auto & module : modules) {
        if (!module || !module->has_summary()) continue;
        root["modules"].push_back(Json::parse(module->summary_json()));
    }
    write_json_file(filename, root);
}

void write_run_summary(const std::string & filename, const CliOptions & opts, const TraceGraph::DagGraph & graph, const Json & stage_timings) {
    // run_summary 是 runner/validation 使用的辅助输出。
    // 用户默认看到的 prediction.json 由 Python runner 从 simulated_e2e_ns 提取。
    Json root;
    root["scenario_name"] = opts.scenario_name;
    root["input_traces"] = opts.input_traces;
    root["graph_output"] = opts.graph_output;
    root["model_config"] = opts.model_config_file;
    root["model_summary"] = opts.model_summary_file;
    root["parsed_record_count"] = graph.parsed_record_count();
    root["simulated_e2e_ns"] = graph.e2e_time();
    root["real_e2e_ns"] = graph.real_e2e_time();
    root["node_count"] = graph.node_count();
    root["edge_count"] = graph.edge_count();
    root["edge_counts_by_kind"] = graph.edge_counts_by_kind();
    root["stage_timings_ms"] = stage_timings;
    write_json_file(filename, root);
}

uint64_t elapsed_ms(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
}

} // namespace

int main(int argc, char ** argv) {
    auto opts = parse_cli(argc, argv);
    if (opts.show_help) {
        print_usage(argv[0]);
        return opts.explicit_help ? 0 : 1;
    }

    auto & logger = TraceGraph::Logger::instance();
    if (opts.debug) {
        logger.set_level(TraceGraph::Logger::DEBUG);
        setenv("DEBUG_TRACE", "1", 1);
    }
    else if (opts.verbose) logger.set_level(TraceGraph::Logger::INFO);
    else logger.set_level(TraceGraph::Logger::WARN);

    try {
        std::vector<TraceGraph::DagGraph> graphs;
        TraceGraph::DagBuilder builder;
        Json timings;
        uint64_t read_ms = 0;
        uint64_t build_ms = 0;
        for (size_t i = 0; i < opts.input_traces.size(); ++i) {
            // 单个输入 trace 独立读入、归一化、构图；这样每个 rank 的 lane/connection 索引不会互相污染。
            auto read_start = std::chrono::steady_clock::now();
            auto events = TraceGraph::read_chrome_trace(opts.input_traces[i]);
            auto read_end = std::chrono::steady_clock::now();
            read_ms += elapsed_ms(read_start, read_end);
            TraceGraph::normalize_trace_events(events);
            auto build_start = std::chrono::steady_clock::now();
            graphs.push_back(builder.build(std::move(events), static_cast<int>(i)));
            auto build_end = std::chrono::steady_clock::now();
            build_ms += elapsed_ms(build_start, build_end);
        }
        timings["read_ms"] = read_ms;
        timings["build_ms"] = build_ms;
        // 多输入 trace 在 per-rank base DAG 构好后 merge，只追加跨 rank 约束。
        auto merge_start = std::chrono::steady_clock::now();
        auto graph = TraceGraph::DagGraph::merge(std::move(graphs));
        auto merge_end = std::chrono::steady_clock::now();
        timings["merge_ms"] = elapsed_ms(merge_start, merge_end);

        auto modules = build_modules(opts.model_config_file);
        auto module_start = std::chrono::steady_clock::now();
        for (const auto & module : modules) {
            logger.info() << "Applying module: " << module->name();
            module->apply(graph);
        }
        auto module_end = std::chrono::steady_clock::now();
        timings["module_ms"] = elapsed_ms(module_start, module_end);

        // 所有模块修改完成后只跑一次拓扑仿真。
        auto simulation_start = std::chrono::steady_clock::now();
        TraceGraph::run_topological_simulation(graph);
        auto simulation_end = std::chrono::steady_clock::now();
        timings["simulation_ms"] = elapsed_ms(simulation_start, simulation_end);
        if (!opts.graph_output.empty()) TraceGraph::write_chrome_trace_dag(opts.graph_output, graph, opts.full_output);
        if (!opts.model_summary_file.empty()) write_module_summary(opts.model_summary_file, modules);
        write_run_summary(opts.run_summary_file, opts, graph, timings);
    }
    catch (const std::exception & e) {
        logger.error() << e.what();
        return 1;
    }
    return 0;
}
