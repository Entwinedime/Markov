/** @file Numerical I/O responses, independent of cache state and execution coordinates. */
#pragma once

#include "markov/trace_graph/frontend/model_config.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace markov::trace_graph::cost {

struct IoServiceCost {
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
[[nodiscard]] const frontend::HiCacheIoServiceModelConfig & require_io_service(const frontend::HiCacheIoCostConfig & config, const std::string & kind,
                                                                               uint64_t page_bytes, uint64_t pages, uint64_t existing_pages = 0);

/** One storage service batch, or a DMA operation group with a known call count.
 * Existing pages still incur host materialization, but do not rewrite the file.
 * The returned integer duration is the clock used by both state replay and DAG.
 * Missing storage-state costs throw MissingCostEvidence; invalid work or an
 * unrepresentable duration returns nullopt.
 */
[[nodiscard]] std::optional<IoServiceCost> estimate_io_service(const frontend::HiCacheIoServiceModelConfig & model, uint64_t page_bytes, uint64_t pages,
                                                               uint64_t calls = 1, uint64_t existing_pages = 0,
                                                               std::optional<uint64_t> copied_pages = std::nullopt);

/** Relative service offsets for one page increment. Cancellation does not
 * undo a copy already executed; its return includes measured cleanup. */
struct PrefetchPageTiming {
    uint64_t publication_offset_us;
    uint64_t cancelled_duration_us;
};

struct PrefetchServiceTiming {
    uint64_t duration_us;
    std::vector<PrefetchPageTiming> publications;
};

/** Uncancelled service and measured prefix-copy alternatives. Returns offsets
 * from service start, not cache visibility decisions or absolute timestamps. */
[[nodiscard]] std::optional<PrefetchServiceTiming> estimate_prefetch_service(const frontend::HiCacheIoServiceModelConfig & model, uint64_t page_bytes,
                                                                             uint64_t pages);

} // namespace markov::trace_graph::cost
