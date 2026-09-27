/**
 * @file
 * @brief Monotonic control clock for target-derived HiCache operations.
 */
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace markov::trace_graph::modules::hicache::runtime {

/**
 * @brief Central monotonic clock for target-derived HiCache control flow.
 *
 * The clock owns operation IDs, enqueue epochs, and boundary epochs. Service schedules
 * own physical completion time; this clock only gives lifecycle records stable order.
 */
class HiCacheTargetControlClock {
public:
    /** @brief Returns the next target-derived operation ID. */
    [[nodiscard]] std::string next_operation_id(std::string_view kind);

    /** @brief Returns the next enqueue epoch. */
    [[nodiscard]] uint64_t next_enqueue_epoch();

    /** @brief Returns the next causal boundary epoch, for facts and finalization alike. */
    [[nodiscard]] uint64_t next_boundary_epoch();

private:
    uint64_t enqueue_epoch_ = 0;
    uint64_t boundary_epoch_ = 0;
    uint64_t operation_epoch_ = 0;
};

} // namespace markov::trace_graph::modules::hicache::runtime
