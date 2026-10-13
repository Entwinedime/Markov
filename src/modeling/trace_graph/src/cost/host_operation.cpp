#include "markov/trace_graph/cost/host_operation.hpp"

#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/cost/operator_model.hpp"
#include "markov/trace_graph/structure/operation.hpp"

#include <cstddef>
#include <string_view>
#include <utility>

namespace markov::trace_graph::cost {

HostOperationCost estimate_host_operation(OperatorCostProvider & provider, structure::OperationWork work, bool submitted,
                                          std::optional<structure::OperationWork> device_work) {
    const OperationCostRequest request{ .work = std::move(work), .worker = submitted, .device = submitted, .device_work = std::move(device_work) };
    return estimate_operation_costs(std::span<const OperationCostRequest>{ &request, 1 }, provider).front().call;
}

std::vector<OperationCost> estimate_operation_costs(std::span<const OperationCostRequest> requests, OperatorCostProvider & provider) {
    // Allocate destinations before taking references. No vector holding these
    // values grows after its references are recorded, and none escapes here.
    std::vector<OperationCost> result(requests.size());
    std::vector<uint64_t> readback_sync_us(requests.size());
    std::vector<structure::OperationWork> work;
    std::vector<uint64_t *> destinations;
    const auto append = [&](structure::OperationWork operation, std::string_view scope, uint64_t & destination) {
        operation.timing_scope = scope;
        work.push_back(std::move(operation));
        destinations.push_back(&destination);
    };
    const auto host = [&](const structure::OperationWork & operation, HostOperationCost & value) {
        append(operation, "cpu_service", value.main_us);
        append(operation, "residual_interval", value.main_residual_us);
    };

    for (size_t i = 0; i < requests.size(); ++i) {
        const auto & request = requests[i];
        auto & estimate = result[i];
        if (request.main_service_work) {
            const auto & service = *request.main_service_work;
            append(service, service.timing_scope, estimate.call.main_us);
            append(request.work, "residual_interval", estimate.call.main_residual_us);
        }
        else { host(request.work, estimate.call); }

        if (request.worker_work) {
            const auto & task = *request.worker_work;
            append(task, "worker_service", estimate.call.worker_us);
            append(task, "worker_residual_interval", estimate.call.worker_residual_us);
            append(task, "dispatch_interval", estimate.call.dispatch_us);
        }
        else if (request.worker) {
            append(request.work, "dispatch_interval", estimate.call.dispatch_us);
            append(request.work, "worker_return_service", estimate.call.worker_return_us);
            append(request.work, "worker_return_residual_interval", estimate.call.worker_return_residual_us);
            if (request.device_launches.empty()) {
                append(request.work, "worker_launch_service", estimate.call.worker_us);
                append(request.work, "worker_launch_residual_interval", estimate.call.worker_residual_us);
            }
        }
        if (!request.device_launches.empty()) {
            estimate.device_launches.resize(request.device_launches.size());
            for (size_t launch = 0; launch < request.device_launches.size(); ++launch) {
                const auto & description = request.device_launches[launch];
                auto & value = estimate.device_launches[launch];
                append(description, "worker_launch_service", value.worker_us);
                append(description, "worker_launch_residual_interval", value.residual_us);
                append(description, "device_service", value.device_us);
            }
        }
        else if (request.device) append(request.device_work ? *request.device_work : request.work, "device_service", estimate.call.device_us);

        if (request.return_work) {
            const auto & tail = *request.return_work;
            auto & value = estimate.tail.emplace();
            if (tail.timing_scope == "blocking_readback_interval") {
                // The dependency already waits for producer work. Estimate
                // only idle synchronization plus the host-clock copy/tail;
                // a measured blocking API wall interval would count it twice.
                append(tail, "post_sync_readback_interval", value.main_us);
                structure::OperationWork synchronization{
                    .component = tail.component,
                    .operation = "runtime.stream_sync",
                    .implementation = "ascend.runtime",
                    .features = { { "previous_work_completed", 1.0 } },
                };
                append(std::move(synchronization), "idle_stream_sync_interval", readback_sync_us[i]);
            }
            else if (tail.timing_scope == "blocking_return_interval") { append(tail, tail.timing_scope, value.main_us); }
            else { host(tail, value); }
        }
    }

    // Resolve the complete admitted program before writing any numeric cost.
    // A missing scope yields all known gaps, never a zero-filled partial plan.
    const auto estimates = provider.estimate_all(work);
    for (size_t i = 0; i < estimates.size(); ++i) *destinations[i] = estimates[i].duration_us;
    for (size_t i = 0; i < requests.size(); ++i) {
        if (requests[i].return_work && requests[i].return_work->timing_scope == "blocking_readback_interval") {
            auto & tail = *result[i].tail;
            tail.main_us = core::checked_add_u64(tail.main_us, readback_sync_us[i], "Readback cost overflow");
        }
    }
    return result;
}

} // namespace markov::trace_graph::cost
