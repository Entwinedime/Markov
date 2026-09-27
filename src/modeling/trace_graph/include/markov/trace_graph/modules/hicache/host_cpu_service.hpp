#pragma once
#include "markov/trace_graph/core/dag_graph.hpp"
#include <nlohmann/json_fwd.hpp>

namespace markov::trace_graph::modules::hicache {
// Resolve exclusive measured host ranges in the source DAG before allocating CPU.
// Return measured recorder writes retained because their source boundary is ambiguous.
nlohmann::json attach_host_cpu_service(core::DagGraph &, const nlohmann::json & measurements);
// Apply measured recorder writes only outside previously corrected source ranges.
// Returns correction totals and retained boundary diagnostics; never removes unknown CPU costs.
nlohmann::json attach_recorder_cpu_service(core::DagGraph &, const nlohmann::json & writes);
} // namespace markov::trace_graph::modules::hicache
