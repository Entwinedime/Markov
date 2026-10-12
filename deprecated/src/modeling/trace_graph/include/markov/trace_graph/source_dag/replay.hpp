#pragma once

#include "markov/trace_graph/core/client_requests.hpp"
#include "markov/trace_graph/core/dag_graph.hpp"
#include "markov/trace_graph/io/trace_manifest_input.hpp"

#include <cstddef>
#include <nlohmann/json_fwd.hpp>
#include <span>
#include <string_view>

namespace markov::trace_graph::source_dag {

/** Report loader selections before inputs move into DAG construction.
 * Does not infer unsupported raw records or change selected events. */
[[nodiscard]] nlohmann::json input_selection_report(std::span<const io::ManifestTraceInput> inputs);

/** Experimental source replay before cost correction and target mutation.
 * Replays a private graph copy; never changes the source or writes artifacts.
 * execution_event_count is the event count before client nodes were attached.
 * Observed requests and the bound chain come from the same source manifest.
 * Reports normalized-source coverage, formal-window HTTP error and per-request
 * latency error (excluding inter-request pauses), not dependency proof.
 * Audits observed completion/return order on existing blocking sync edges;
 * does not adjust timestamps or infer missing edges from the replay error.
 */
[[nodiscard]] nlohmann::json replay_report(const core::DagGraph & source, const core::ClientRequestChain & chain,
                                           std::span<const core::ClientRequestTiming> requests, size_t execution_event_count, std::string_view manifest);

} // namespace markov::trace_graph::source_dag
