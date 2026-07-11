/**
 * @file
 * @brief Typed command-line contract for the TraceGraph executable.
 */
#pragma once

#include "markov/trace_graph/io/trace_manifest_input.hpp"

#include <optional>
#include <stdexcept>
#include <string>

namespace markov::trace_graph::cli {

/** @brief Output paths enabled by the current build type and command line. */
struct OutputPaths {
    std::string run_summary;
    std::string graph;
#ifdef DEBUG
    std::string model_summary;
    std::string dag_analysis_directory;
#endif
};

/** @brief Fully parsed options consumed by workflow orchestration. */
struct CliOptions {
    std::string profile_manifest;
    std::string model_config;
    io::ManifestTraceInputOptions trace_input;
    OutputPaths outputs;
    bool debug_logging = false;
    bool verbose_logging = false;
};

/** @brief Distinguishes invalid user syntax from backend execution failures. */
class CliUsageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/** @brief Parses and validates CLI arguments; returns empty only for help requests. */
[[nodiscard]] std::optional<CliOptions> parse_cli_options(int argc, char ** argv);

/** @brief Prints build-appropriate usage text to standard output. */
void print_usage(const char * program);

} // namespace markov::trace_graph::cli
