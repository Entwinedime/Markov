#include "trace_graph/activity_record.hpp"
#include "trace_graph/domains/cache_io/cache_model.hpp"
#include "trace_graph/export_raw_trace.hpp"
#include "trace_graph/frontend/model_config.hpp"
#include "trace_graph/frontend/trace_normalizer.hpp"
#include "trace_graph/logger.hpp"
#include "trace_graph/optimization/scale_transform.hpp"
#include "trace_graph/simulation/topological_simulator.hpp"
#include "trace_graph/trace_dag.hpp"
#include "trace_graph/trace_parser.hpp"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

struct CliOptions {
    std::vector<std::string> input_traces;
    std::string output_file = "output_graph.json";
    std::vector<std::pair<std::string, double>> scales;
    bool debug = false;
    bool verbose = false;
    bool full_output = false;
    bool no_raw = false;
    bool explicit_help = false;
    bool show_help = false;
    std::string model_config_file;
    std::string model_summary_file;
};

void print_usage(const char * prog) {
    std::cout << "Usage: " << prog << " [OPTIONS] <trace_file> [trace_file ...]\n\n"
              << "Build a DAG from Ascend trace JSON and run topological simulation.\n\n"
              << "Options:\n"
              << "  -h, --help                  Show this help message\n"
              << "  -o, --output FILE           Output file (default: output_graph.json)\n"
              << "  -s, --scale NAME=FACTOR[,...]\n"
              << "                              Time-scaling operations (repeatable, comma-separated)\n"
              << "                              Example: --scale \"CPUInfer::sync=0.5\"\n"
              << "                              Example: --scale \"CPUInfer::sync=0.5,aclrtMemcpyAsync=2.0\"\n"
              << "  -d, --debug                 Export intermediate debug files\n"
              << "  -v, --verbose               Show progress output\n"
              << "  --full-output               Generate full Chrome tracing with edge flows\n"
              << "  --no-raw                    Skip raw parsed trace export\n"
              << "  --model-config FILE         Apply domain model config, e.g. cache_io tiers\n"
              << "  --model-summary FILE        Model summary JSON output\n\n"
              << "All --scale values are applied together, then a single simulation runs.\n"
              << "Multiple trace files are treated as separate GPU cards and merged.\n\n"
              << "Environment:\n"
              << "  TRACE_GRAPH_COLOR=0         Disable colored log output\n";
}

std::vector<std::pair<std::string, double>> parse_scale_spec(const std::string & spec) {
    std::vector<std::pair<std::string, double>> result;
    std::istringstream iss(spec);
    std::string token;
    while (std::getline(iss, token, ',')) {
        auto eq = token.find('=');
        if (eq == std::string::npos) {
            std::cerr << "Error: --scale expects NAME=FACTOR format, got '" << token << "'\n";
            continue;
        }
        std::string name = token.substr(0, eq);
        double factor = std::stod(token.substr(eq + 1));
        result.emplace_back(std::move(name), factor);
    }
    return result;
}

CliOptions parse_cli(int argc, char ** argv) {
    CliOptions opts;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            opts.explicit_help = true;
            opts.show_help = true;
            return opts;
        }
        else if (arg == "-o" || arg == "--output") {
            if (i + 1 < argc) opts.output_file = argv[++i];
            else {
                std::cerr << "Error: --output requires a value\n";
                opts.show_help = true;
                return opts;
            }
        }
        else if (arg == "-s" || arg == "--scale") {
            if (i + 1 < argc) {
                auto parsed = parse_scale_spec(argv[++i]);
                for (auto & p : parsed) opts.scales.push_back(std::move(p));
            }
            else {
                std::cerr << "Error: --scale requires a value\n";
                opts.show_help = true;
                return opts;
            }
        }
        else if (arg == "-d" || arg == "--debug") { opts.debug = true; }
        else if (arg == "-v" || arg == "--verbose") { opts.verbose = true; }
        else if (arg == "--full-output") { opts.full_output = true; }
        else if (arg == "--no-raw") { opts.no_raw = true; }
        else if (arg == "--model-config") {
            if (i + 1 < argc) opts.model_config_file = argv[++i];
            else {
                std::cerr << "Error: --model-config requires a value\n";
                opts.show_help = true;
                return opts;
            }
        }
        else if (arg == "--model-summary") {
            if (i + 1 < argc) opts.model_summary_file = argv[++i];
            else {
                std::cerr << "Error: --model-summary requires a value\n";
                opts.show_help = true;
                return opts;
            }
        }
        else if (arg[0] == '-') {
            std::cerr << "Error: Unknown option: " << arg << "\n";
            opts.show_help = true;
            return opts;
        }
        else { opts.input_traces.push_back(arg); }
    }

    if (opts.input_traces.empty()) {
        std::cerr << "Error: No input trace files specified.\n";
        opts.show_help = true;
    }
    return opts;
}

} // namespace

int main(int argc, char ** argv) {
    CliOptions opts = parse_cli(argc, argv);

    if (opts.show_help) {
        print_usage(argv[0]);
        return opts.explicit_help ? 0 : 1;
    }

    auto & logger = TraceGraph::Logger::instance();
    if (opts.debug) {
        logger.set_level(TraceGraph::Logger::DEBUG);
        setenv("DEBUG_TRACE", "1", 1);
    }
    else if (opts.verbose) { logger.set_level(TraceGraph::Logger::INFO); }
    else { logger.set_level(TraceGraph::Logger::WARN); }

    try {
        std::vector<TraceGraph::TraceDAG> graphs;
        int gpu_id = 0;

        for (const auto & trace_file : opts.input_traces) {
            logger.info() << "Loading trace from: " << trace_file << " as GPU " << gpu_id;

            std::vector<std::unique_ptr<TraceGraph::ActivityRecord>> records;
            TraceGraph::parse_trace_json(trace_file, records);
            TraceGraph::normalize_trace_records(records);

            if (records.empty()) {
                logger.warn() << "No records parsed for " << trace_file << ". Skipping.";
                continue;
            }

            logger.info() << "  Parsed " << records.size() << " records.";

            if (!opts.no_raw) {
                std::string raw_output = "raw_parsed_" + std::to_string(gpu_id) + ".json";
                TraceGraph::export_raw_trace(raw_output, records);
                logger.info() << "  Exported raw trace to " << raw_output;
            }

            logger.info() << "  Building DAG...";
            graphs.push_back(TraceGraph::TraceDAG::from_records(std::move(records), gpu_id));
            gpu_id++;
        }

        if (graphs.empty()) {
            logger.error() << "No valid graphs built. Exiting.";
            return 1;
        }

        logger.info() << "Merging " << graphs.size() << " graphs...";
        TraceGraph::TraceDAG merged = TraceGraph::TraceDAG::merge_graphs(graphs);

        for (const auto & kv : opts.scales) {
            logger.info() << "Scaling '" << kv.first << "' by " << kv.second;
            TraceGraph::apply_scale_transform(merged, kv.first, kv.second);
        }

        bool wrote_model_summary = false;
        if (!opts.model_config_file.empty()) {
            TraceGraph::ModelConfig model_config = TraceGraph::ModelConfig::from_file(opts.model_config_file);
            if (model_config.cache_io.enabled) {
                auto summary = TraceGraph::apply_cache_io_model(merged, model_config.cache_io);
                std::string summary_file = opts.model_summary_file.empty() ? opts.output_file + ".model_summary.json" : opts.model_summary_file;
                summary.write_json(summary_file);
                logger.info() << "Exported model summary to " << summary_file;
                wrote_model_summary = true;
            }
        }
        else if (!opts.model_summary_file.empty()) {
            logger.warn() << "--model-summary ignored because --model-config was not provided.";
        }

        logger.info() << "Running simulation...";
        TraceGraph::run_topological_simulation(merged);

        logger.info() << "End-to-End time: " << merged.e2e_time() << " ns";

        merged.to_chrome_tracing_json(opts.output_file, /*concise=*/true, opts.full_output);
        logger.info() << "Exported to " << opts.output_file;
        if (!wrote_model_summary && !opts.model_config_file.empty()) {
            logger.warn() << "Model config did not enable any implemented domain.";
        }
    }
    catch (const std::exception & e) {
        logger.error() << e.what();
        return 1;
    }

    return 0;
}
