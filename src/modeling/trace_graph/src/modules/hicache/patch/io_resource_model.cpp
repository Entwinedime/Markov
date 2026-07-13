/**
 * @file
 * @brief Read-only HiCache I/O resource-model implementation.
 */
#include "markov/trace_graph/modules/hicache/patch/io_resource_model.hpp"

#include "markov/trace_graph/core/numeric.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <ranges>
#include <string_view>

namespace markov::trace_graph::modules::hicache::patch {

namespace io_resource_model_detail {

using model::HiCacheEffectDecision;
using model::HiCacheEffectDecisionLedger;
using model::HiCacheEffectType;
using model::HiCacheTargetEffectState;
using model::HiCacheTransferDirection;

constexpr std::string_view kResourceModelId = "scope_local_directional_device_host_shared_host_storage_v1";
constexpr std::array<std::string_view, 3> kProvenanceFields{
    "kv_geometry",
    "device_host_bandwidth",
    "host_storage_bandwidth",
};

struct ResourceSelection {
    std::string field;
    uint64_t value = 0;
    std::string lane;
};

ResourceSelection resources_for(const HiCacheEffectDecision & decision, const HiCacheEffectDecisionLedger & ledger) {
    switch (decision.direction) {
    case HiCacheTransferDirection::StorageToHost:
    case HiCacheTransferDirection::HostToStorage:
        return ResourceSelection{
            .field = "host_storage_bandwidth_bytes_per_sec",
            .value = ledger.host_storage_bandwidth_bytes_per_sec,
            .lane = "host_storage_lane",
        };
    case HiCacheTransferDirection::HostToDevice:
        return ResourceSelection{
            .field = "device_host_bandwidth_bytes_per_sec",
            .value = ledger.device_host_bandwidth_bytes_per_sec,
            .lane = "host_to_device_lane",
        };
    case HiCacheTransferDirection::DeviceToHost:
        return ResourceSelection{
            .field = "device_host_bandwidth_bytes_per_sec",
            .value = ledger.device_host_bandwidth_bytes_per_sec,
            .lane = "device_to_host_lane",
        };
    case HiCacheTransferDirection::None:
        return {};
    }
    return {};
}

bool complete_provenance(const std::map<std::string, std::string> & provenance) {
    if (provenance.size() != kProvenanceFields.size()) return false;
    return std::ranges::all_of(kProvenanceFields, [&](std::string_view field) {
        const auto it = provenance.find(std::string(field));
        return it != provenance.end() && !it->second.empty();
    });
}

HiCacheIoCostRecord cost_record(const HiCacheEffectDecision & decision, const HiCacheEffectDecisionLedger & ledger) {
    HiCacheIoCostRecord record{
        .effect_id = decision.effect_key,
        .effect_type = decision.effect_type,
        .direction = decision.direction,
        .target_effect_state = decision.target_effect_state,
        .effective_byte_count = decision.effective_byte_count,
        .resource_scope = decision.cache_scope,
        .logical_order_epoch = decision.eligibility_boundary.epoch,
    };
    if (decision.target_effect_state == HiCacheTargetEffectState::NotRequired) {
        record.status = HiCacheIoCostStatus::NotRequired;
        record.reason = "target_effect_not_required";
        return record;
    }
    if (decision.target_effect_state == HiCacheTargetEffectState::Deferred) {
        record.status = HiCacheIoCostStatus::Deferred;
        record.reason = "target_effect_deferred";
        return record;
    }
    if (decision.target_effect_state == HiCacheTargetEffectState::Unresolved) {
        record.status = HiCacheIoCostStatus::Unresolved;
        record.reason = "target_effect_unresolved";
        return record;
    }
    if (decision.direction == HiCacheTransferDirection::None) {
        if (decision.effect_type == HiCacheEffectType::CommitCapacityGate || decision.effect_type == HiCacheEffectType::PrefetchVisibility) {
            record.status = HiCacheIoCostStatus::Ready;
            return record;
        }
        record.status = HiCacheIoCostStatus::UnsupportedDirection;
        record.reason = "unsupported_transfer_direction";
        return record;
    }
    if (decision.effective_page_count == 0) {
        record.status = HiCacheIoCostStatus::MissingEffectiveBytes;
        record.reason = "missing_effective_pages";
        return record;
    }
    if (!ledger.byte_projection_available) {
        record.status = HiCacheIoCostStatus::MissingByteProjection;
        record.reason = "missing_byte_projection";
        return record;
    }
    if (decision.effective_byte_count == 0) {
        record.status = HiCacheIoCostStatus::ByteProjectionOverflow;
        record.reason = "byte_projection_overflow";
        return record;
    }

    const auto resources = resources_for(decision, ledger);
    record.bandwidth_field = resources.field;
    record.bandwidth_bytes_per_sec = resources.value;
    if (resources.field.empty() || resources.lane.empty()) {
        record.status = HiCacheIoCostStatus::UnsupportedDirection;
        record.reason = "unsupported_transfer_direction";
        return record;
    }
    if (record.resource_scope.empty()) {
        record.status = HiCacheIoCostStatus::MissingResourceScope;
        record.reason = "missing_resource_scope";
        return record;
    }
    record.resource_lane = record.resource_scope + "/" + resources.lane;
    if (ledger.io_model_id.empty() || ledger.io_model_digest.empty()) {
        record.status = HiCacheIoCostStatus::MissingIoModelIdentity;
        record.reason = "missing_io_model_identity";
        return record;
    }
    if (ledger.resource_model.empty()) {
        record.status = HiCacheIoCostStatus::MissingResourceModel;
        record.reason = "missing_resource_model";
        return record;
    }
    if (ledger.resource_model != kResourceModelId) {
        record.status = HiCacheIoCostStatus::UnsupportedResourceModel;
        record.reason = "unsupported_resource_model";
        return record;
    }
    if (!complete_provenance(ledger.io_model_provenance)) {
        record.status = HiCacheIoCostStatus::MissingIoModelProvenance;
        record.reason = "missing_io_model_provenance";
        return record;
    }
    if (resources.value == 0) {
        record.status = HiCacheIoCostStatus::MissingBandwidth;
        record.reason = "missing_" + resources.field;
        return record;
    }
    const auto projected_duration = core::ceil_multiply_divide_u64(decision.effective_byte_count, 1'000'000, resources.value);
    if (!projected_duration) {
        record.status = HiCacheIoCostStatus::DurationOverflow;
        record.reason = "io_duration_overflow";
        return record;
    }
    record.duration_us = *projected_duration;
    record.status = HiCacheIoCostStatus::Ready;
    return record;
}

void append_lane_dependencies(HiCacheIoResourcePlan & plan) {
    std::map<std::string, std::vector<const HiCacheIoCostRecord *>> by_lane;
    for (const auto & cost : plan.costs) {
        if (cost.status == HiCacheIoCostStatus::Ready && !cost.resource_lane.empty()) by_lane[cost.resource_lane].push_back(&cost);
    }
    for (auto & [lane, costs] : by_lane) {
        std::ranges::sort(costs, [](const auto * left, const auto * right) {
            if (left->logical_order_epoch != right->logical_order_epoch) return left->logical_order_epoch < right->logical_order_epoch;
            return left->effect_id < right->effect_id;
        });
        for (size_t index = 1; index < costs.size(); ++index) {
            plan.lane_dependencies.push_back(HiCacheIoLaneDependency{
                .resource_lane = lane,
                .predecessor_effect_id = costs[index - 1]->effect_id,
                .successor_effect_id = costs[index]->effect_id,
            });
        }
    }
}

} // namespace io_resource_model_detail

uint64_t HiCacheIoResourcePlan::ready_count() const {
    const auto it = counts_by_status.find("ready");
    return it == counts_by_status.end() ? 0 : it->second;
}

bool HiCacheIoResourcePlan::calibrated_for_apply() const { return io_model_calibration_status == "calibrated"; }

std::string hicache_io_cost_status_name(HiCacheIoCostStatus status) {
    switch (status) {
    case HiCacheIoCostStatus::Ready:
        return "ready";
    case HiCacheIoCostStatus::NotRequired:
        return "not_required";
    case HiCacheIoCostStatus::Deferred:
        return "deferred";
    case HiCacheIoCostStatus::Unresolved:
        return "unresolved";
    case HiCacheIoCostStatus::MissingEffectiveBytes:
        return "missing_effective_bytes";
    case HiCacheIoCostStatus::MissingByteProjection:
        return "missing_byte_projection";
    case HiCacheIoCostStatus::ByteProjectionOverflow:
        return "byte_projection_overflow";
    case HiCacheIoCostStatus::MissingIoModelIdentity:
        return "missing_io_model_identity";
    case HiCacheIoCostStatus::MissingResourceModel:
        return "missing_resource_model";
    case HiCacheIoCostStatus::UnsupportedResourceModel:
        return "unsupported_resource_model";
    case HiCacheIoCostStatus::MissingIoModelProvenance:
        return "missing_io_model_provenance";
    case HiCacheIoCostStatus::MissingResourceScope:
        return "missing_resource_scope";
    case HiCacheIoCostStatus::MissingBandwidth:
        return "missing_bandwidth";
    case HiCacheIoCostStatus::DurationOverflow:
        return "duration_overflow";
    case HiCacheIoCostStatus::UnsupportedDirection:
        return "unsupported_direction";
    }
    return "unknown";
}

HiCacheIoResourcePlan build_hicache_io_resource_plan(const model::HiCacheEffectDecisionLedger & decisions) {
    HiCacheIoResourcePlan plan;
    plan.io_model_id = decisions.io_model_id;
    plan.io_model_digest = decisions.io_model_digest;
    plan.io_model_calibration_status = decisions.io_model_calibration_status;
    plan.resource_model_id = decisions.resource_model;
    plan.byte_projection_source = decisions.byte_projection_source;
    plan.kv_bytes_per_page = decisions.kv_bytes_per_page;
    plan.device_host_bandwidth_bytes_per_sec = decisions.device_host_bandwidth_bytes_per_sec;
    plan.host_storage_bandwidth_bytes_per_sec = decisions.host_storage_bandwidth_bytes_per_sec;
    plan.io_model_provenance = decisions.io_model_provenance;
    plan.costs.reserve(decisions.decisions.size());
    for (const auto & decision : decisions.decisions) {
        auto cost = io_resource_model_detail::cost_record(decision, decisions);
        const auto status = hicache_io_cost_status_name(cost.status);
        (void)core::checked_increment_u64(plan.counts_by_status[status], "HiCache I/O cost status count exceeds uint64 range");
        if (!cost.reason.empty() && cost.status != HiCacheIoCostStatus::Ready && cost.status != HiCacheIoCostStatus::NotRequired) {
            (void)core::checked_increment_u64(plan.blocker_counts[cost.reason], "HiCache I/O cost blocker count exceeds uint64 range");
        }
        plan.costs.push_back(std::move(cost));
    }
    io_resource_model_detail::append_lane_dependencies(plan);
    if (plan.costs.empty()) plan.status = "no_effect_decisions";
    else if (plan.blocker_counts.empty()) plan.status = "ready";
    else if (plan.ready_count() > 0) plan.status = "partial";
    else plan.status = "blocked";
    return plan;
}

} // namespace markov::trace_graph::modules::hicache::patch
