/**
 * @file
 * @brief Debug-only DAG analysis artifact builders.
 */
#pragma once

#include "markov/trace_graph/core/dag_graph.hpp"
#include "markov/trace_graph/core/dag_mutation.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

namespace markov::trace_graph::modules::dag_analysis {

/** @brief Four JSON artifacts produced by the first-stage DAG analysis pass. */
struct DagAnalysisArtifacts {
    std::string dag_quality_json;
    std::string dag_analysis_json;
    std::string dag_anchor_coverage_json;
    std::string dag_operation_visibility_json;
    std::map<std::string, uint64_t> timings_ms;
};

/**
 * @brief Builds first-stage analysis artifacts from a successfully simulated DAG.
 *
 * This operation is read-only and may be expensive; it never changes nodes, edges,
 * durations, or module state.
 */
[[nodiscard]] DagAnalysisArtifacts build_dag_analysis_artifacts(const core::DagGraph & graph, size_t threads = 1);

/**
 * @brief Builds a structured witness for a cycle failure.
 *
 * The validation path uses this after simulation fails, preserving auditable node and edge
 * evidence that identifies which dependency rules closed the cycle.
 */
[[nodiscard]] std::string build_cycle_witness_json(const core::DagGraph & graph, const std::string & error_message = "");

/** @brief Builds a structured witness for mutation-plan or topology validation failure. */
[[nodiscard]] std::string build_topology_validation_json(const core::DagGraph & graph, const core::DagTopologyValidationReport & report,
                                                         const std::string & error_message = "");

} // namespace markov::trace_graph::modules::dag_analysis
