/**
 * @file
 * @brief Declares the stable run-summary artifact boundary.
 */
#pragma once

#include "markov/trace_graph/core/dag_graph.hpp"
#include "markov/trace_graph/modules/module.hpp"

#include <memory>
#include <string>
#include <vector>

namespace markov::trace_graph::cli {

/** @brief Writes Release-compatible graph and module results to the required path. */
void write_run_summary(const std::string & filename, const core::DagGraph & graph, const std::vector<std::unique_ptr<modules::SimulationModule>> & modules);

} // namespace markov::trace_graph::cli
