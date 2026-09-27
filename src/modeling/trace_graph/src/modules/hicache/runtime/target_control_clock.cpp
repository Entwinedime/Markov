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
    return core::checked_increment_u64(enqueue_epoch_, "HiCache enqueue epoch exceeds uint64 range");
}

uint64_t HiCacheTargetControlClock::next_boundary_epoch() {
    return core::checked_increment_u64(boundary_epoch_, "HiCache boundary epoch exceeds uint64 range");
}

} // namespace markov::trace_graph::modules::hicache::runtime
