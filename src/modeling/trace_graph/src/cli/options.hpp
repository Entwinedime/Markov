/**
 * @file
 * @brief Typed command-line contract for the TraceGraph executable.
 */
#pragma once

#include "markov/trace_graph/io/trace_manifest_input.hpp"

#include <cstdint>
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
#endif
};

/** @brief Fully parsed options consumed by workflow orchestration. */
struct CliOptions {
    std::string profile_manifest;
    std::string model_config;
#ifdef DEBUG
    std::string hicache_oracle_cost_replay;
#endif
    io::ManifestTraceInputOptions trace_input;
#ifdef DEBUG
    std::optional<uint64_t> actual_e2e_us;
#endif
    OutputPaths outputs;
    bool debug_logging = false;
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
