/** @file Serializes source-observed HiCache I/O work for model construction. */
#pragma once

#include "markov/trace_graph/core/dag_graph.hpp"
#include "markov/trace_graph/modules/hicache/patch/io_operation_ledger.hpp"

#include <nlohmann/json_fwd.hpp>

namespace markov::trace_graph::cli {

[[nodiscard]] nlohmann::json hicache_io_observations(
    const core::DagGraph & graph,
    const modules::hicache::patch::HiCacheIoOperationLedger & operations);

} // namespace markov::trace_graph::cli
