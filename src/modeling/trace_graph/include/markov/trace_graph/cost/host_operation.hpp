#pragma once

#include "markov/trace_graph/structure/operation.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace markov::trace_graph::cost {

class OperatorCostProvider;

/** Numeric service scopes, independent of the runtime that binds them.
 * Zero initializes an absent structural role; present roles require evidence.
 * All fields are microseconds. worker_us is service through the single launch
 * boundary, or the whole CPU-only task when there is no device launch;
 * worker_return_us is remaining service in a launched task.
 * Their residual intervals stay separate; dispatch is charged once.
 * Queue/device waits belong to dependencies. */
struct HostOperationCost {
    uint64_t main_us = 0, main_residual_us = 0;
    uint64_t worker_us = 0, dispatch_us = 0, device_us = 0;
    uint64_t worker_residual_us = 0, worker_return_us = 0, worker_return_residual_us = 0;
};

/** Resolve independent service scopes for already-described work. No graph,
 * source template, resource selection or topology mutation is accepted.
 * submitted=false means the structure has no worker/device operation, not
 * missing evidence; CPU service and residual still require separate models.
 * All returned durations are microseconds. Missing required scopes propagate.
 */
[[nodiscard]] HostOperationCost estimate_host_operation(OperatorCostProvider & provider, structure::OperationWork work, bool submitted,
                                                        std::optional<structure::OperationWork> device_work = std::nullopt);

/** Numeric requirements of an already-generated host/device operation. No graph
 * IDs, resource identities or dependencies cross into the cost provider.
 * Absent roles require no samples; present roles must have independent costs.
 * return_work selects cpu_service (plus residual_interval), the observed
 * blocking_return_interval, or a readback assembled from independently
 * measured idle synchronization and the post-sync copy/tail interval.
 * Readbacks approximate ordinary/infinite-timeout Ascend synchronization;
 * dependency waiting and wake-up latency are not part of the idle sample.
 */
struct OperationCostRequest {
    structure::OperationWork work;
    bool worker = false, device = false;
    std::optional<structure::OperationWork> return_work;
    std::vector<structure::OperationWork> device_launches;
    // A known native device primitive may be shared across different callers.
    // It replaces only the single device service, never CPU/worker scopes.
    std::optional<structure::OperationWork> device_work;
    // A blocking synchronization has its own idle API service; the caller's
    // residual remains separate, and dependency waiting is never charged here.
    std::optional<structure::OperationWork> main_service_work;
    // A CPU-only queued primitive has full worker service, not an invented
    // device launch/return. It uses its own work and dispatch observations.
    std::optional<structure::OperationWork> worker_work;
};

struct DeviceLaunchCost {
    uint64_t worker_us, residual_us, device_us;
};

struct OperationCost {
    HostOperationCost call;
    // Blocking tails hold the selected post-wait interval in main_us. A
    // synchronous readback includes memcpy and is not pure CPU service.
    std::optional<HostOperationCost> tail;
    std::vector<DeviceLaunchCost> device_launches;
};

/** Estimate one numeric record per request, preserving request order. Does
 * not select execution branches, create tasks or alter their dependencies.
 * Every scope in the admitted program is queried before returning results;
 * missing scopes are reported together without partial numeric binding. */
[[nodiscard]] std::vector<OperationCost> estimate_operation_costs(std::span<const OperationCostRequest> requests, OperatorCostProvider & provider);

} // namespace markov::trace_graph::cost
