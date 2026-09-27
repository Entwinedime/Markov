/** @file Serializes source-observed HiCache I/O work for model construction. */
#pragma once

#include "markov/trace_graph/core/dag_graph.hpp"
#include "markov/trace_graph/modules/hicache/patch/io_operation_ledger.hpp"
#include "markov/trace_graph/modules/hicache/phase_observation.hpp"

#include <nlohmann/json_fwd.hpp>

namespace markov::trace_graph::cli {

[[nodiscard]] nlohmann::json hicache_phase_observations(const modules::hicache::HiCachePhaseObservationAudit & phase);

[[nodiscard]] nlohmann::json hicache_io_observations(const core::DagGraph & graph, const modules::hicache::patch::HiCacheIoOperationLedger & operations,
                                                     bool cpu_service_applied);

} // namespace markov::trace_graph::cli
