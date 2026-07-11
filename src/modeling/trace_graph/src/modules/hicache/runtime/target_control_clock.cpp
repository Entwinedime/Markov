/**
 * @file
 * @brief Target-derived HiCache control-clock implementation.
 */
#include "markov/trace_graph/modules/hicache/runtime/target_control_clock.hpp"

#include "markov/trace_graph/core/numeric.hpp"

#include <stdexcept>

namespace markov::trace_graph::modules::hicache::runtime {

/** @brief Allocates a readable monotonic ID for a target-derived operation. */
std::string HiCacheTargetControlClock::next_operation_id(std::string_view kind) {
    if (kind.empty()) throw std::invalid_argument("HiCache operation kind must not be empty");
    const auto epoch = core::checked_increment_u64(operation_epoch_, "HiCache operation ID epoch exceeds uint64 range");
    return std::string(kind) + ":" + std::to_string(epoch);
}

/**
 * @brief Allocates a monotonic enqueue epoch.
 *
 * This model-local clock explains causal order and does not represent source scheduling.
 */
uint64_t HiCacheTargetControlClock::next_enqueue_epoch() {
#ifdef DEBUG
    (void)core::checked_increment_u64(scheduler_epoch_, "HiCache scheduler epoch exceeds uint64 range");
#endif
    return core::checked_increment_u64(enqueue_epoch_, "HiCache enqueue epoch exceeds uint64 range");
}

/** @brief Records the terminal target boundary used to settle remaining operations. */
uint64_t HiCacheTargetControlClock::record_target_finalize_boundary(const std::string & cache_scope, uint64_t ts) { return record_boundary(cache_scope, ts); }

/**
 * @brief Allocates one boundary epoch and, in Debug, records its provenance.
 *
 * Production callers consume only the epoch. Debug builds retain the descriptive record
 * needed to reproduce boundary order in summaries.
 */
uint64_t HiCacheTargetControlClock::record_boundary(const std::string & cache_scope, uint64_t ts) {
    const auto boundary_epoch = core::checked_increment_u64(boundary_epoch_, "HiCache boundary epoch exceeds uint64 range");
#ifdef DEBUG
    boundaries_.push_back(HiCacheControlBoundary{
        .scheduler_epoch = core::checked_increment_u64(scheduler_epoch_, "HiCache scheduler epoch exceeds uint64 range"),
        .boundary_epoch = boundary_epoch,
        .cache_scope = cache_scope,
        .request_key = "",
        .kind = "finalize",
        .source = "target_finalize",
        .source_event_index = 0,
        .ts = ts,
        .terminal = true,
    });
#else
    (void)cache_scope;
    (void)ts;
#endif
    return boundary_epoch;
}

} // namespace markov::trace_graph::modules::hicache::runtime
