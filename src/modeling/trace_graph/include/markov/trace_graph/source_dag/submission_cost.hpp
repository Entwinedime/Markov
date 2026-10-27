#pragma once

#include "markov/trace_graph/adapters/frameworks/sglang/hicache_observations.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_expansion.hpp"
#include "markov/trace_graph/structure/operation.hpp"

#include <cstddef>
#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <span>
#include <string_view>

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

/** Append the measured scopes of one operation. Absent measurements remain
 * absent rows, not zeros. Source position plus timing scope is the import
 * identity; work describes variables, not an executable calibration template.
 * CPU scopes are labeled profiled unless this capture has paired correction.
 * submitted=false emits caller scopes only. */
void append_host_operation_observations(nlohmann::json & rows, const structure::OperationWork & work, std::string_view position,
                                        const HostOperationMeasurement & measured, bool corrected, bool submitted);

/** Measure the supported unsorted allocation followed by promoted-node clones.
 * Work lengths come from the source admission, never equal division by clone
 * count. Requires one complete asynchronous task/device per submission and no
 * unowned device work. Unsupported/incomplete layouts return no measurements.
 * An empty clone_lengths is admissible only for a single full-length clone.
 * Durations retain corrected CPU/residual scopes and fractional device time. */
[[nodiscard]] std::optional<nlohmann::json> load_index_operator_observations(const modules::hicache::runtime::HiCacheHostExpansion & plan,
                                                                             const adapters::sglang::HiCacheSourceIndex & source,
                                                                             const simulation::detail::CpuTaskQueues & queues, uint64_t tokens,
                                                                             uint64_t page_size, std::span<const uint64_t> clone_lengths,
                                                                             std::string_view position, bool corrected);

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
                                                                                const adapters::sglang::HiCacheSourceIndex & source,
                                                                                const simulation::detail::CpuTaskQueues & queues, bool paged = true);

[[nodiscard]] std::optional<PageFirstSubmissionMeasurement> observe_page_first_submission_cost(const modules::hicache::runtime::HiCacheWriteExpansion & plan,
                                                                                               const adapters::sglang::HiCacheSourceIndex & source,
                                                                                               const simulation::detail::CpuTaskQueues & queues,
                                                                                               size_t completion_count);

} // namespace markov::trace_graph::source_dag
