/**
 * @file
 * @brief Debug-only TraceGraph CLI diagnostics boundary.
 */
#pragma once

#ifdef DEBUG
#include "markov/trace_graph/core/dag_graph.hpp"
#include "markov/trace_graph/core/dag_mutation.hpp"
#include "markov/trace_graph/core/logger.hpp"
#include "markov/trace_graph/modules/module.hpp"

#include <exception>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace markov::trace_graph::cli {

/**
 * @brief Writes summaries exposed by modules after they have executed.
 *
 * The writer is available only in diagnostics builds. Release builds neither
 * expose module summaries nor link their JSON serializers.
 */
void write_module_summary(const std::string & filename, const std::vector<std::unique_ptr<modules::SimulationModule>> & modules);

/**
 * @brief Writes the complete set of DAG analysis artifacts.
 *
 * Artifact generation traverses the graph and can be expensive, so callers
 * must invoke it only when an explicit output directory was requested.
 */
std::map<std::string, uint64_t> write_dag_analysis_artifacts(const std::string & output_dir, const core::DagGraph & graph, size_t threads = 1);

/** @brief Writes a topology or cycle witness for a failed workflow. */
void write_dag_failure_artifact(const std::string & output_dir, const core::DagGraph & graph, const std::exception & error, core::Logger & logger);

} // namespace markov::trace_graph::cli
#endif
