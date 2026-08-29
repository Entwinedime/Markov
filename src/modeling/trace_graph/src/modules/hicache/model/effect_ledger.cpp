/** @file Compact HiCache effect-ledger counts and enum names. */
#include "markov/trace_graph/modules/hicache/model/effect_decision.hpp"

#include "markov/trace_graph/core/numeric.hpp"

#include <algorithm>
#include <ranges>

namespace markov::trace_graph::modules::hicache::model {

uint64_t HiCacheEffectDecisionLedger::patchable_count() const {
    return static_cast<uint64_t>(
        std::ranges::count_if(decisions, [](const auto & decision) { return decision.patch_status == HiCacheEffectPatchStatus::Patchable; }));
}

uint64_t HiCacheEffectDecisionLedger::not_patchable_count() const {
    return static_cast<uint64_t>(
        std::ranges::count_if(decisions, [](const auto & decision) { return decision.patch_status == HiCacheEffectPatchStatus::NotPatchable; }));
}

uint64_t HiCacheEffectDecisionLedger::deferred_count() const {
    return static_cast<uint64_t>(
        std::ranges::count_if(decisions, [](const auto & decision) { return decision.patch_status == HiCacheEffectPatchStatus::Deferred; }));
}

uint64_t HiCacheEffectDecisionLedger::unresolved_count() const {
    return static_cast<uint64_t>(
        std::ranges::count_if(decisions, [](const auto & decision) { return decision.target_effect_state == HiCacheTargetEffectState::Unresolved; }));
}

uint64_t HiCacheEffectDecisionLedger::schedule_sensitive_count() const {
    return static_cast<uint64_t>(std::ranges::count_if(decisions, [](const auto & decision) {
        return decision.schedule_sensitivity == HiCacheScheduleSensitivity::ArrivalScheduleSensitive;
    }));
}

std::map<std::string, uint64_t> HiCacheEffectDecisionLedger::counts_by_effect_type() const {
    std::map<std::string, uint64_t> counts;
    for (const auto & decision : decisions)
        (void)core::checked_increment_u64(counts[hicache_effect_type_name(decision.effect_type)], "HiCache effect type count exceeds uint64 range");
    return counts;
}

std::map<std::string, uint64_t> HiCacheEffectDecisionLedger::counts_by_target_effect_state() const {
    std::map<std::string, uint64_t> counts;
    for (const auto & decision : decisions)
        (void)core::checked_increment_u64(counts[hicache_target_effect_state_name(decision.target_effect_state)],
                                          "HiCache target effect-state count exceeds uint64 range");
    return counts;
}

std::map<std::string, uint64_t> HiCacheEffectDecisionLedger::counts_by_schedule_sensitivity() const {
    std::map<std::string, uint64_t> counts;
    for (const auto & decision : decisions)
        (void)core::checked_increment_u64(counts[hicache_schedule_sensitivity_name(decision.schedule_sensitivity)],
                                          "HiCache schedule-sensitivity count exceeds uint64 range");
    return counts;
}

std::map<std::string, uint64_t> HiCacheEffectDecisionLedger::counts_by_source_carrier_state() const {
    std::map<std::string, uint64_t> counts;
    for (const auto & decision : decisions)
        (void)core::checked_increment_u64(counts[hicache_source_carrier_state_name(decision.source_carrier_state)],
                                          "HiCache source carrier-state count exceeds uint64 range");
    return counts;
}

std::string hicache_effect_type_name(HiCacheEffectType type) {
    switch (type) {
    case HiCacheEffectType::Loadback: return "loadback";
    case HiCacheEffectType::PrefetchIo: return "prefetch_io_operation";
    case HiCacheEffectType::PrefetchVisibility: return "prefetch_visibility_dependency";
    case HiCacheEffectType::CommitDeviceToHost: return "commit_device_to_host";
    case HiCacheEffectType::CommitHostToStorage: return "commit_host_to_storage";
    case HiCacheEffectType::CommitCapacityGate: return "commit_capacity_gate";
    }
    return "unknown";
}

std::string hicache_transfer_direction_name(HiCacheTransferDirection direction) {
    switch (direction) {
    case HiCacheTransferDirection::None: return "none";
    case HiCacheTransferDirection::StorageToHost: return "storage_to_host";
    case HiCacheTransferDirection::HostToDevice: return "host_to_device";
    case HiCacheTransferDirection::DeviceToHost: return "device_to_host";
    case HiCacheTransferDirection::HostToStorage: return "host_to_storage";
    }
    return "unknown";
}

std::string hicache_effect_patch_status_name(HiCacheEffectPatchStatus status) {
    switch (status) {
    case HiCacheEffectPatchStatus::Patchable: return "patchable";
    case HiCacheEffectPatchStatus::NotPatchable: return "not_patchable";
    case HiCacheEffectPatchStatus::Deferred: return "deferred";
    }
    return "unknown";
}

std::string hicache_target_effect_state_name(HiCacheTargetEffectState state) {
    switch (state) {
    case HiCacheTargetEffectState::Required: return "required";
    case HiCacheTargetEffectState::NotRequired: return "not_required";
    case HiCacheTargetEffectState::Partial: return "partial";
    case HiCacheTargetEffectState::Deferred: return "deferred";
    case HiCacheTargetEffectState::Unresolved: return "unresolved";
    }
    return "unknown";
}

std::string hicache_schedule_sensitivity_name(HiCacheScheduleSensitivity sensitivity) {
    switch (sensitivity) {
    case HiCacheScheduleSensitivity::ScheduleInvariant: return "schedule_invariant";
    case HiCacheScheduleSensitivity::ArrivalScheduleSensitive: return "arrival_schedule_sensitive";
    }
    return "unknown";
}

std::string hicache_source_carrier_state_name(HiCacheSourceCarrierState state) {
    switch (state) {
    case HiCacheSourceCarrierState::NotEvaluated: return "not_evaluated";
    case HiCacheSourceCarrierState::Present: return "present";
    case HiCacheSourceCarrierState::Absent: return "absent";
    case HiCacheSourceCarrierState::Unobservable: return "unobservable";
    case HiCacheSourceCarrierState::Ambiguous: return "ambiguous";
    }
    return "unknown";
}

} // namespace markov::trace_graph::modules::hicache::model
