/** @file Source-observed client work around a serial HTTP request stream. */
#pragma once

#include "markov/trace_graph/core/dag_graph.hpp"
#include <optional>
#include <span>

namespace markov::trace_graph::core {

struct ClientRequestTiming {
    std::string request_id;
    uint64_t start_us = 0;
    uint64_t end_us = 0;
    uint64_t frontend_end_us = 0;
    /** Declared generation reservation, not measured output length. Missing
     * workload metadata cannot supply an admission budget. */
    std::optional<uint64_t> generation_reservation_tokens;
};

struct ClientRequestNodes {
    std::string request_id;
    size_t start = 0;
    size_t completion = 0;
    std::optional<uint64_t> generation_reservation_tokens;
};

struct ClientRequestChain {
    std::string status = "missing_requests";
    std::vector<ClientRequestNodes> requests;
    /** Source workload proves quiescence before the first executed request. */
    std::optional<uint64_t> hicache_idle_since_us;
    /** Earlier requests prepare cache state but are outside the HTTP measurement. */
    size_t formal_begin_index = 0;
};

/** Attach only after every serial request has unique, ordered source boundaries.
 * Request observations must first be inserted as synthetic CPU-gap boundaries.
 * Costs are measured source frontend/response/client work, never target labels.
 * Existing server work and residual waits remain; this does not certify accuracy.
 */
[[nodiscard]] ClientRequestChain connect_client_requests(DagGraph & graph, std::span<const ClientRequestTiming> requests,
                                                         std::optional<uint64_t> hicache_idle_since_us = std::nullopt, size_t formal_begin_index = 0);

} // namespace markov::trace_graph::core
