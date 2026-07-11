/**
 * @file
 * @brief Streaming Chrome trace JSON reader and DAG writer.
 */
#pragma once

#include "markov/trace_graph/core/dag_graph.hpp"
#include "markov/trace_graph/core/trace_event.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace markov::trace_graph::io {

/** @brief Execution and filtering options for one Chrome trace file. */
struct TraceReadOptions {
    /** @brief Retains metadata needed to associate CANN processes with native sidecars. */
    bool include_metadata = false;

    /** @brief Retains validation-only duration events that normally stay outside the DAG. */
    bool include_validation_only = false;

    /** @brief Retains duration events explicitly ignored by faithful replay. */
    bool include_ignored_duration = false;

    /** @brief Repairs only the known unclosed suffix produced by interrupted streaming writers. */
    bool auto_repair = false;

    /** @brief Number of parser partitions inside one file; one selects the serial scanner. */
    size_t threads = 1;
};

/**
 * @brief Reads executable Chrome trace duration events with default options.
 *
 * The scanner retains only top-level fields needed by DAG construction and a shared lazy
 * slice for `args`; it never materializes a whole-file JSON DOM.
 */
[[nodiscard]] std::vector<core::TraceEvent> read_chrome_trace(const std::string & filename);

/** @brief Reads Chrome trace events using explicit filtering and parser options. */
[[nodiscard]] std::vector<core::TraceEvent> read_chrome_trace(const std::string & filename, const TraceReadOptions & options);

/**
 * @brief Writes the active simulated DAG as a Chrome trace with flow dependencies.
 */
void write_chrome_trace_dag(const std::string & filename, const core::DagGraph & graph);

} // namespace markov::trace_graph::io
