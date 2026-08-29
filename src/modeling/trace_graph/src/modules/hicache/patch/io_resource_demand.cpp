/**
 * @file
 * @brief Operation-family naming and deterministic lane ordering.
 */
#include "io_resource_demand.hpp"

#include <algorithm>
#include <map>
#include <ranges>
#include <vector>

namespace markov::trace_graph::modules::hicache::patch::io_resource_model_detail {

std::string operation_kind(const model::HiCacheEffectDecision & decision) {
    using model::HiCacheEffectType;
    switch (decision.effect_type) {
    case HiCacheEffectType::PrefetchIo:
        return "prefetch";
    case HiCacheEffectType::Loadback:
        return "load";
    case HiCacheEffectType::CommitDeviceToHost:
        return "write_device_to_host";
    case HiCacheEffectType::CommitHostToStorage:
        return "write_host_to_storage";
    case HiCacheEffectType::PrefetchVisibility:
    case HiCacheEffectType::CommitCapacityGate:
        return {};
    }
    return {};
}

void append_lane_dependencies(HiCacheIoResourcePlan & plan) {
    std::map<std::string, std::vector<const HiCacheIoCostRecord *>> by_lane;
    for (const auto & cost : plan.costs) {
        if (cost.status == HiCacheIoCostStatus::Ready && cost.effective_byte_count > 0 && cost.duration_us > 0 && !cost.resource_lane.empty())
            by_lane[cost.resource_lane].push_back(&cost);
    }
    for (auto & [lane, costs] : by_lane) {
        std::ranges::sort(costs, [](const auto * left, const auto * right) {
            if (left->logical_order_epoch != right->logical_order_epoch) return left->logical_order_epoch < right->logical_order_epoch;
            return left->effect_id < right->effect_id;
        });
        for (size_t index = 1; index < costs.size(); ++index) {
            plan.lane_dependencies.push_back({
                .resource_lane = lane,
                .predecessor_effect_id = costs[index - 1]->effect_id,
                .successor_effect_id = costs[index]->effect_id,
            });
        }
    }
}

} // namespace markov::trace_graph::modules::hicache::patch::io_resource_model_detail
