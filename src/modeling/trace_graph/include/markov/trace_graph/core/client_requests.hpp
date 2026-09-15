/** @file Source-observed client work around a serial HTTP request stream. */
#pragma once

#include "markov/trace_graph/core/dag_graph.hpp"
#include <span>

namespace markov::trace_graph::core {

struct ClientRequestTiming {
    std::string request_id;
    uint64_t start_us = 0;
    uint64_t end_us = 0;
    uint64_t frontend_end_us = 0;
};

struct ClientRequestNodes {
    std::string request_id;
    size_t start = 0;
    size_t completion = 0;
};

struct ClientRequestChain {
    std::string status = "missing_requests";
    std::vector<ClientRequestNodes> requests;
};

/** Attach only after every serial request has unique, ordered source boundaries.
 * Costs are measured source frontend/response/client work, never target labels.
 * Existing server work and residual waits remain; this does not certify accuracy.
 */
[[nodiscard]] ClientRequestChain connect_client_requests(DagGraph & graph, std::span<const ClientRequestTiming> requests);

} // namespace markov::trace_graph::core
