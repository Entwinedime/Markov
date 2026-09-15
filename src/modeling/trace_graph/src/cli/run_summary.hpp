/**
 * @file
 * @brief Declares the stable run-summary artifact boundary.
 */
#pragma once

#include "markov/trace_graph/core/dag_graph.hpp"
#include "markov/trace_graph/modules/module.hpp"
#include "markov/trace_graph/modules/hicache/phase_observation.hpp"
#include <nlohmann/json_fwd.hpp>

#include <memory>
#include <string>
#include <vector>

namespace markov::trace_graph::cli {

/** @brief Writes Release-compatible graph and module results to the required path. */
void write_run_summary(const std::string & filename, const core::DagGraph & graph,
                       const std::vector<std::unique_ptr<modules::SimulationModule>> & modules,
                       const nlohmann::json & source_io_observations,
                       const modules::hicache::HiCachePhaseObservationAudit & source_phase_observations,
                       const nlohmann::json & client_result);

} // namespace markov::trace_graph::cli
