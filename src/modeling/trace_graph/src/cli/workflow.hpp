/**
 * @file
 * @brief Public CLI orchestration boundary for one TraceGraph backend run.
 */
#pragma once

namespace markov::trace_graph::core {
class Logger;
}

namespace markov::trace_graph::cli {

struct CliOptions;

/**
 * @brief Executes trace loading, DAG construction, modules, simulation, and output.
 *
 * User-input and runtime failures propagate as exceptions to `main`; successful
 * execution returns zero after the required run summary has been written.
 */
int run_workflow(const CliOptions & options, core::Logger & logger);

} // namespace markov::trace_graph::cli
