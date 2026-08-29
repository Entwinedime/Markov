/**
 * @file
 * @brief Monotonic control clock for target-derived HiCache operations.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace markov::trace_graph::modules::hicache::runtime {

/**
 * @brief Debug record for one target-control boundary.
 *
 * The target model generates these records to explain state-machine ordering. They never
 * represent source-observed completion.
 */

/**
 * @brief Central monotonic clock for target-derived HiCache control flow.
 *
 * The clock owns operation IDs, enqueue epochs, and boundary epochs. Acknowledgements are
 * still folded synchronously, but control ordering no longer leaks across operation tables.
 */
class HiCacheTargetControlClock {
public:
    /** @brief Returns the next target-derived operation ID. */
    [[nodiscard]] std::string next_operation_id(std::string_view kind);

    /** @brief Returns the next enqueue epoch. */
    [[nodiscard]] uint64_t next_enqueue_epoch();

    /** @brief Records a target-finalize boundary and returns its causal epoch. */
    [[nodiscard]] uint64_t record_target_finalize_boundary(const std::string & cache_scope, uint64_t ts);

    /** @brief Records one canonical fact boundary consumed by a target async operation. */
    [[nodiscard]] uint64_t record_fact_boundary(const std::string & cache_scope, const std::string & request_key, const std::string & kind,
                                                size_t source_event_index, uint64_t ts);


private:
    uint64_t enqueue_epoch_ = 0;
    uint64_t boundary_epoch_ = 0;
    uint64_t operation_epoch_ = 0;

    [[nodiscard]] uint64_t record_boundary(const std::string & cache_scope, const std::string & request_key, const std::string & kind,
                                           const std::string & source, size_t source_event_index, uint64_t ts, bool terminal);
};

} // namespace markov::trace_graph::modules::hicache::runtime
