#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace markov::trace_graph::modules::hicache {

/** Completion coordinates supplied by execution/cost binding, in microseconds.
 * A cancelled increment publishes nothing, but still executes its copy and
 * return path. Successful observations do not measure that cancelled return.
 * Existing type names remain stable for static and execution consumers.
 */
struct PrefetchPublication {
    uint64_t page_count, at_us;
    std::optional<uint64_t> cancelled_return_us;
};

struct PrefetchControlBatch {
    uint64_t page_count, start_ts, ready_ts; // Absolute microsecond coordinates.
    // Ordered increments: pagewise for generic file, grouped for atomic paths.
    // Never divide a whole service duration into guessed publication costs.
    std::vector<PrefetchPublication> publications;
};

struct PrefetchBatchExecution {
    uint64_t copied_pages = 0, published_pages = 0, release_us = 0;
    bool cancelled = false;
};

} // namespace markov::trace_graph::modules::hicache

namespace markov::trace_graph::structure::hicache {

/** File-worker cancellation rule, independent of cost coefficients. The worker
 * reads the whole batch, then tests cancellation after each copy. Cancellation
 * during cleanup after all publications succeeded does not cancel the batch.
 */
[[nodiscard]] modules::hicache::PrefetchBatchExecution execute_prefetch_batch(const modules::hicache::PrefetchControlBatch & batch,
                                                                              std::optional<uint64_t> stop_us);

} // namespace markov::trace_graph::structure::hicache
