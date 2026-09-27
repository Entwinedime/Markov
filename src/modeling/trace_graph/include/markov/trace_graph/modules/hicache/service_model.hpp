/** @file Shared I/O service primitives for cache-state time and DAG costs. */
#pragma once

#include "markov/trace_graph/frontend/model_config.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace markov::trace_graph::modules::hicache {

/** An increment after physical page writes. A cancelled increment publishes
 * nothing, but its copy and return path still execute. The caller supplies that
 * branch's return time explicitly; successful observations do not measure it.
 */
struct PrefetchPublication {
    uint64_t page_count, at_us;
    std::optional<uint64_t> cancelled_return_us;
};

struct PrefetchControlBatch {
    uint64_t page_count, start_ts, ready_ts;
    // Ordered increments: pagewise for generic file, grouped for atomic paths.
    // No default that divides a whole service duration into guessed page costs.
    std::vector<PrefetchPublication> publications;
};

struct PrefetchBatchExecution {
    uint64_t copied_pages = 0, published_pages = 0, release_us = 0;
    bool cancelled = false;
};

/** The worker reads the whole batch before testing cancellation at increment.
 * A rejected increment still pays its copy. Cancellation during cleanup after
 * all increments succeeded does not stop the next batch from being read.
 */
[[nodiscard]] PrefetchBatchExecution execute_prefetch_batch(const PrefetchControlBatch & batch, std::optional<uint64_t> stop_us);

struct HiCacheServiceCost {
    uint64_t duration_us = 0;
    double setup_us = 0.0;
    double transfer_us = 0.0;
    double existing_us = 0.0;
    double new_us = 0.0;
    double bandwidth_bytes_per_sec = 0.0;
};

/** Look up a service only after positive transfer work has been established.
 * Missing evidence reports the shared service and byte/page coordinates; it
 * never substitutes zero or changes the operation that requested the cost.
 * For storage writes, pages and existing_pages describe the first batch.
 * The returned reference remains owned by the supplied configuration.
 */
[[nodiscard]] const frontend::HiCacheIoServiceModelConfig & require_hicache_service(const frontend::HiCacheIoCostConfig & config, const std::string & kind,
                                                                                    uint64_t page_bytes, uint64_t pages, uint64_t existing_pages = 0);

/** One storage service batch, or a DMA operation group with a known call count.
 * Existing pages still incur host materialization, but do not rewrite the file.
 * The returned integer duration is the clock used by both state replay and DAG.
 * Missing storage-state costs throw MissingCostEvidence; invalid work or an
 * unrepresentable duration returns nullopt.
 */
[[nodiscard]] std::optional<HiCacheServiceCost> hicache_service_cost(const frontend::HiCacheIoServiceModelConfig & model, uint64_t page_bytes, uint64_t pages,
                                                                     uint64_t calls = 1, uint64_t existing_pages = 0,
                                                                     std::optional<uint64_t> copied_pages = std::nullopt);

/** Full uncancelled service with page publications and measured batch cleanup.
 * No timestamps are fabricated when the calibration lacks stage observations.
 * The common copy/publication primitive covers both increment branches; the
 * controller chooses the actual prefix and resource release from this timeline.
 */
[[nodiscard]] std::optional<PrefetchControlBatch> hicache_prefetch_batch(const frontend::HiCacheIoServiceModelConfig & model, uint64_t page_bytes,
                                                                         uint64_t pages, uint64_t start_us);

[[nodiscard]] std::string hicache_resource_lane(const frontend::HiCacheIoCostConfig & model, const std::string & kind, const std::string & scope);

} // namespace markov::trace_graph::modules::hicache
