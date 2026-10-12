#pragma once

#include "markov/trace_graph/modules/hicache/runtime/write_expansion.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace markov::trace_graph::source_dag {

/** Measured microseconds, not a ready-to-bind prediction. Each missing scope
 * stays null; measured zero is valid. CPU/residual and worker/device evidence
 * are admitted independently and cannot stand in for one another. */
struct HostOperationMeasurement {
    std::optional<uint64_t> main_us, main_residual_us;
    std::optional<uint64_t> worker_us, worker_residual_us, worker_return_us, worker_return_residual_us;
    std::optional<uint64_t> dispatch_us;
    // Preserve raw device precision before model fitting or simulation rounding.
    std::optional<double> device_us;
};

struct PagedReleaseMeasurement {
    HostOperationMeasurement before_first_sync, first_sync, before_second_sync, second_sync, concatenate, tail;
};

struct PageFirstSubmissionMeasurement {
    HostOperationMeasurement prepare_sync, index_sync, start_record, wait_event;
    HostOperationMeasurement first_copy, copy, first_completion_record, completion_record, tail;
};

/** Partition a complete observed release/submission into service scopes.
 * Plans retain source-node identities from this immutable source graph.
 * Unsupported layouts return no record; unmeasurable scopes remain absent.
 * source CPU correction is applied without turning waits into service.
 * Repeated primitive values are summarized within this one observed call.
 * These functions export measurements, never generate a target program. */
[[nodiscard]] std::optional<PagedReleaseMeasurement> observe_paged_release_cost(const modules::hicache::runtime::HiCacheHostExpansion & plan,
                                                                                const modules::hicache::patch::HiCacheSourceDagIndex & source,
                                                                                const simulation::detail::CpuTaskQueues & queues, bool paged = true);

[[nodiscard]] std::optional<PageFirstSubmissionMeasurement> observe_page_first_submission_cost(const modules::hicache::runtime::HiCacheWriteExpansion & plan,
                                                                                               const modules::hicache::patch::HiCacheSourceDagIndex & source,
                                                                                               const simulation::detail::CpuTaskQueues & queues,
                                                                                               size_t completion_count);

} // namespace markov::trace_graph::source_dag
