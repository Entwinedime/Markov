#include "markov/trace_graph/core/numeric.hpp"
#include "markov/trace_graph/modules/hicache/runtime/host_program.hpp"

#include <string>
#include <vector>

namespace markov::trace_graph::modules::hicache::runtime {

void bind_worker_segment(core::DagSyntheticNodeSpec & worker, uint64_t service_us, uint64_t residual_us, std::string_view source) {
    worker.duration = core::checked_add_u64(service_us, residual_us, "Worker segment interval overflow");
    worker.attrs["worker_service_us"] = std::to_string(service_us);
    worker.attrs["worker_residual_us"] = std::to_string(residual_us);
    worker.attrs["cost_binding_source"] = source;
}

void bind_host_operation(HiCacheHostExpansion & plan, const HostOperationSlots & slots, const cost::HostOperationCost & cost, std::string_view source) {
    auto & main = plan.nodes[slots.main].work;
    main.duration = core::checked_multiply_u64(cost.main_us, slots.main_repetitions, "Repeated caller service overflow");
    main.cpu_gap_after = core::checked_multiply_u64(cost.main_residual_us, slots.main_repetitions, "Repeated caller residual overflow");
    main.attrs["cost_binding_source"] = source;

    if (slots.worker) {
        auto & worker = plan.nodes[*slots.worker].work;
        bind_worker_segment(worker, cost.worker_us, cost.worker_residual_us, source);
        worker.cpu_task_ready_delay_us = cost.dispatch_us;
    }
    if (slots.worker_return) {
        auto & returned = plan.nodes[*slots.worker_return].work;
        bind_worker_segment(returned, cost.worker_return_us, cost.worker_return_residual_us, source);
    }
    if (slots.device) {
        auto & device = plan.nodes[*slots.device].work;
        device.duration = cost.device_us;
        device.attrs["cost_binding_source"] = source;
    }
}

void bind_host_operations(HiCacheHostExpansion & plan, std::span<const HostOperationBinding> operations, cost::OperatorCostProvider & provider,
                          std::string_view source) {
    std::vector<cost::OperationCostRequest> requests;
    requests.reserve(operations.size());
    for (const auto & operation : operations) requests.push_back(operation.request);

    const auto costs = cost::estimate_operation_costs(requests, provider);
    for (size_t i = 0; i < operations.size(); ++i) {
        for (const auto & slots : operations[i].slots) {
            if (slots.device_launches.empty()) { bind_host_operation(plan, slots, costs[i].call, source); }
            else {
                bind_host_operation(plan, { .main = slots.main, .worker_return = slots.worker_return }, costs[i].call, source);
                for (size_t launch = 0; launch < slots.device_launches.size(); ++launch) {
                    const auto & target = slots.device_launches[launch];
                    const auto & estimate = costs[i].device_launches[launch];
                    auto & worker = plan.nodes[target.worker].work;
                    bind_worker_segment(worker, estimate.worker_us, estimate.residual_us, source);
                    if (launch == 0) worker.cpu_task_ready_delay_us = costs[i].call.dispatch_us;

                    auto & device = plan.nodes[target.device].work;
                    device.duration = estimate.device_us;
                    device.attrs["cost_binding_source"] = source;
                }
            }
            if (slots.tail) bind_host_operation(plan, { .main = *slots.tail }, costs[i].tail.value(), source);
        }
    }
}

} // namespace markov::trace_graph::modules::hicache::runtime
