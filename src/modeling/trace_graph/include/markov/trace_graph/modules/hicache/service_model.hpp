/** @file Shared I/O service primitives for cache-state time and DAG costs. */
#pragma once

#include "markov/trace_graph/frontend/model_config.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace markov::trace_graph::modules::hicache {

struct HiCacheServiceCost {
    uint64_t duration_us = 0;
    double setup_us = 0.0;
    double transfer_us = 0.0;
    double existing_us = 0.0;
    double new_us = 0.0;
    double bandwidth_bytes_per_sec = 0.0;
};

/** One storage service batch, or a DMA operation group with a known call count.
 * Existing pages still incur host materialization, but do not rewrite the file.
 * The returned integer duration is the clock used by both state replay and DAG.
 */
[[nodiscard]] std::optional<HiCacheServiceCost> hicache_service_cost(
    const frontend::HiCacheIoServiceModelConfig & model, uint64_t page_bytes,
    uint64_t pages, uint64_t calls = 1, uint64_t existing_pages = 0);

[[nodiscard]] std::string hicache_resource_lane(const frontend::HiCacheIoCostConfig & model,
                                               const std::string & kind, const std::string & scope);

} // namespace markov::trace_graph::modules::hicache
