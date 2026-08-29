/**
 * @file
 * @brief Parses TraceGraph command-line arguments into typed workflow options.
 */
#include "options.hpp"

#include "markov/trace_graph/core/numeric.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <limits>
#include <string_view>

namespace markov::trace_graph::cli {

namespace {

std::string next_value(int & index, int argc, char ** argv, std::string_view option) {
    if (index + 1 >= argc) throw CliUsageError(std::string(option) + " requires a value");
    return argv[++index];
}

size_t positive_size(std::string_view raw, std::string_view option) {
    const auto value = core::parse_exact_u64(raw);
    if (!value || *value == 0 || *value > std::numeric_limits<size_t>::max()) { throw CliUsageError(std::string(option) + " expects a positive integer"); }
    return static_cast<size_t>(*value);
}

uint64_t nonnegative_u64(std::string_view raw, std::string_view option) {
    const auto value = core::parse_exact_u64(raw);
    if (value) return *value;
    throw CliUsageError(std::string(option) + " expects a non-negative integer");
}

std::string normalized_token(std::string_view raw) {
    size_t begin = 0;
    size_t end = raw.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(raw[begin])) != 0) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(raw[end - 1])) != 0) --end;
    if (begin == end) return {};

    std::string token(raw.substr(begin, end - begin));
    std::ranges::transform(token, token.begin(), [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return token;
}

void select_trace_channels(io::ManifestTraceInputOptions & options, const std::string & raw) {
    options.include_torch = false;
    options.include_ld_preload = false;
    options.include_python_probe = false;

    size_t begin = 0;
    while (begin <= raw.size()) {
        const auto end = raw.find(',', begin);
        const auto length = end == std::string::npos ? std::string::npos : end - begin;
        const auto token = normalized_token(std::string_view(raw).substr(begin, length));
        if (token == "all") {
            options.include_torch = true;
            options.include_ld_preload = true;
            options.include_python_probe = true;
        }
        else if (token == "torch") options.include_torch = true;
        else if (token == "ld_preload") options.include_ld_preload = true;
        else if (token == "python_probe") options.include_python_probe = true;
        else if (!token.empty()) throw CliUsageError("--trace-channels contains unknown channel: " + token);

        if (end == std::string::npos) break;
        begin = end + 1;
    }

    if (!options.include_torch && !options.include_ld_preload && !options.include_python_probe) {
        throw CliUsageError("--trace-channels must enable at least one channel");
    }
}

void validate_options(const CliOptions & options) {
    if (options.profile_manifest.empty()) throw CliUsageError("--profile-manifest is required");
    if (options.outputs.run_summary.empty()) throw CliUsageError("--run-summary is required");
    if (options.trace_input.window_start_us.has_value() != options.trace_input.window_end_us.has_value()) {
        throw CliUsageError("--trace-window-start-us and --trace-window-end-us must be provided together");
    }
    if (options.trace_input.window_start_us && *options.trace_input.window_end_us <= *options.trace_input.window_start_us) {
        throw CliUsageError("trace window end must be greater than start");
    }
}

} // namespace

std::optional<CliOptions> parse_cli_options(int argc, char ** argv) {
    CliOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "-h" || argument == "--help") return std::nullopt;
        if (argument == "--profile-manifest") options.profile_manifest = next_value(index, argc, argv, argument);
        else if (argument == "--threads") options.trace_input.threads = positive_size(next_value(index, argc, argv, argument), argument);
        else if (argument == "--file-threads") options.trace_input.file_threads = positive_size(next_value(index, argc, argv, argument), argument);
        else if (argument == "--trace-window-start-us") {
            options.trace_input.window_start_us = nonnegative_u64(next_value(index, argc, argv, argument), argument);
        }
        else if (argument == "--trace-window-end-us") {
            options.trace_input.window_end_us = nonnegative_u64(next_value(index, argc, argv, argument), argument);
        }
        else if (argument == "--trace-channels") select_trace_channels(options.trace_input, next_value(index, argc, argv, argument));
        else if (argument == "--graph-output") options.outputs.graph = next_value(index, argc, argv, argument);
        else if (argument == "--model-config") options.model_config = next_value(index, argc, argv, argument);
#ifdef DEBUG
        else if (argument == "--actual-e2e-us") { options.actual_e2e_us = nonnegative_u64(next_value(index, argc, argv, argument), argument); }
        else if (argument == "--hicache-oracle-cost-replay") options.hicache_oracle_cost_replay = next_value(index, argc, argv, argument);
        else if (argument == "--model-summary") options.outputs.model_summary = next_value(index, argc, argv, argument);
#endif
        else if (argument == "--run-summary") options.outputs.run_summary = next_value(index, argc, argv, argument);
        else if (argument == "-d" || argument == "--debug") options.debug_logging = true;
        else throw CliUsageError("unknown option: " + std::string(argument));
    }
    validate_options(options);
    return options;
}

void print_usage(const char * program) {
    std::cout << "Usage: " << program << " --profile-manifest FILE --run-summary FILE [OPTIONS]\n\n"
              << "Build a DAG directly from profile traces and run topological simulation.\n\n"
              << "Input:\n"
              << "  --profile-manifest FILE         profile_manifest.json input\n"
              << "  --trace-channels LIST           torch,ld_preload,python_probe; default all\n"
              << "  --threads N                     Total logical input read/build thread budget\n"
              << "  --file-threads N                Per-file JSON parse threads\n"
              << "\n"
              << "  --trace-window-start-us N       Inclusive workload-window start timestamp\n"
              << "  --trace-window-end-us N         Inclusive workload-window end timestamp\n\n"
              << "Model and output:\n"
              << "  --model-config FILE             Optional SimulationModule config\n"
              << "  --graph-output FILE             Optional full DAG Chrome trace\n"
#ifdef DEBUG
              << "  --actual-e2e-us N               Explicit workload E2E used by validation\n"
              << "  --hicache-oracle-cost-replay FILE  Diagnostic target-observed effect costs\n"
              << "  --model-summary FILE            Optional module validation summary\n"
#endif
              << "  --run-summary FILE              Required compact run summary\n"
              << "  -d, --debug                     Debug logging\n"
              << "  -h, --help                      Show this help\n";
}

} // namespace markov::trace_graph::cli
