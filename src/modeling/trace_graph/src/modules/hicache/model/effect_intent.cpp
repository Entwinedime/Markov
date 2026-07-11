/**
 * @file
 * @brief Derives HiCache effect intents from canonical asynchronous operation tables.
 */
#include "markov/trace_graph/modules/hicache/model/state.hpp"

#include "markov/trace_graph/core/numeric.hpp"

#include <algorithm>
#include <limits>
#include <ranges>
#include <utility>

namespace markov::trace_graph::modules::hicache::model {

namespace effect_intent_detail {

std::string operation_state_name(HiCacheOperationState state) {
    switch (state) {
    case HiCacheOperationState::Created:
        return "created";
    case HiCacheOperationState::Queued:
        return "queued";
    case HiCacheOperationState::Ready:
        return "ready";
    case HiCacheOperationState::Completed:
        return "completed";
    case HiCacheOperationState::Committed:
        return "committed";
    case HiCacheOperationState::Cancelled:
        return "cancelled";
    }
    return "unknown";
}

std::string prefetch_state_name(HiCachePrefetchState state) {
    switch (state) {
    case HiCachePrefetchState::Pending:
        return "pending";
    case HiCachePrefetchState::Ready:
        return "ready";
    case HiCachePrefetchState::Applied:
        return "applied";
    case HiCachePrefetchState::Suppressed:
        return "suppressed";
    case HiCachePrefetchState::Late:
        return "late";
    case HiCachePrefetchState::Revoked:
        return "revoked";
    }
    return "unknown";
}

uint64_t projected_bytes(uint64_t page_count, uint64_t kv_bytes_per_page) {
    if (page_count == 0 || kv_bytes_per_page == 0 || page_count > std::numeric_limits<uint64_t>::max() / kv_bytes_per_page) return 0;
    return page_count * kv_bytes_per_page;
}

HiCacheEffectBoundary start_boundary(const HiCacheOperationHeader & header) {
    return HiCacheEffectBoundary{
        .kind = "enqueue",
        .epoch = header.enqueue_epoch,
        .timestamp_us = header.enqueue_ts,
    };
}

HiCacheEffectBoundary completion_boundary(const HiCacheOperationHeader & header) {
    if (header.complete_epoch > 0)
        return HiCacheEffectBoundary{
            .kind = "completion",
            .epoch = header.complete_epoch,
            .timestamp_us = header.complete_ts,
        };
    return HiCacheEffectBoundary{
        .kind = "terminal_boundary_unresolved",
        .epoch = header.boundary_epoch,
        .timestamp_us = header.boundary_ts,
    };
}

std::string effect_id(const std::string & cache_scope, HiCacheEffectType type, const std::string & operation_id) {
    return hicache_effect_type_name(type) + ":" + cache_scope + ":" + operation_id;
}

void mark_contract_skeleton_status(HiCacheEffectIntent & intent, const HiCacheEffectIntentCatalog & catalog) {
    intent.patch_status = HiCacheEffectPatchStatus::NotPatchable;
    if (intent.effective_page_count == 0) intent.not_patchable_reason = "missing_effective_pages";
    else if (!catalog.byte_projection_available) intent.not_patchable_reason = "missing_byte_projection";
    else if (intent.effective_byte_count == 0) intent.not_patchable_reason = "byte_projection_overflow";
    else intent.not_patchable_reason = "source_attribution_not_implemented";
}

HiCacheEffectIntent intent_from_header(const HiCacheOperationHeader & header, HiCacheEffectType type, HiCacheTransferDirection direction,
                                       std::string resource_lane, std::vector<std::string> effective_pages, uint64_t candidate_page_count,
                                       const HiCacheEffectIntentCatalog & catalog) {
    HiCacheEffectIntent intent{
        .effect_id = effect_id(header.cache_scope, type, header.operation_id),
        .effect_type = type,
        .operation_id = header.operation_id,
        .request_key = header.request_key,
        .cache_scope = header.cache_scope,
        .direction = direction,
        .pages = std::move(effective_pages),
        .candidate_page_count = candidate_page_count,
        .logical_start_boundary = start_boundary(header),
        .logical_completion_boundary = completion_boundary(header),
        .consumer_boundary = HiCacheEffectBoundary{ .kind = "source_attribution_unresolved" },
        .resource_lane = std::move(resource_lane),
        .state = operation_state_name(header.state),
        .not_patchable_reason = {},
    };
    intent.effective_page_count = static_cast<uint64_t>(intent.pages.size());
    intent.effective_byte_count = projected_bytes(intent.effective_page_count, catalog.kv_bytes_per_page);
    mark_contract_skeleton_status(intent, catalog);
    return intent;
}

} // namespace effect_intent_detail

using effect_intent_detail::intent_from_header;
using effect_intent_detail::prefetch_state_name;

uint64_t HiCacheEffectIntentCatalog::patchable_count() const {
    return static_cast<uint64_t>(
        std::ranges::count_if(intents, [](const auto & intent) { return intent.patch_status == HiCacheEffectPatchStatus::Patchable; }));
}

uint64_t HiCacheEffectIntentCatalog::not_patchable_count() const {
    return static_cast<uint64_t>(
        std::ranges::count_if(intents, [](const auto & intent) { return intent.patch_status == HiCacheEffectPatchStatus::NotPatchable; }));
}

uint64_t HiCacheEffectIntentCatalog::deferred_count() const {
    return static_cast<uint64_t>(std::ranges::count_if(intents, [](const auto & intent) { return intent.patch_status == HiCacheEffectPatchStatus::Deferred; }));
}

std::map<std::string, uint64_t> HiCacheEffectIntentCatalog::counts_by_effect_type() const {
    std::map<std::string, uint64_t> counts;
    for (const auto & intent : intents) {
        auto & count = counts[hicache_effect_type_name(intent.effect_type)];
        (void)core::checked_increment_u64(count, "HiCache effect type count exceeds uint64 range");
    }
    return counts;
}

std::string hicache_effect_type_name(HiCacheEffectType type) {
    switch (type) {
    case HiCacheEffectType::Prefetch:
        return "prefetch";
    case HiCacheEffectType::Loadback:
        return "loadback";
    case HiCacheEffectType::CommitDeviceToHost:
        return "commit_device_to_host";
    case HiCacheEffectType::CommitHostToStorage:
        return "commit_host_to_storage";
    case HiCacheEffectType::CommitCapacityGate:
        return "commit_capacity_gate";
    case HiCacheEffectType::Prefill:
        return "prefill";
    }
    return "unknown";
}

std::string hicache_transfer_direction_name(HiCacheTransferDirection direction) {
    switch (direction) {
    case HiCacheTransferDirection::None:
        return "none";
    case HiCacheTransferDirection::StorageToHost:
        return "storage_to_host";
    case HiCacheTransferDirection::HostToDevice:
        return "host_to_device";
    case HiCacheTransferDirection::DeviceToHost:
        return "device_to_host";
    case HiCacheTransferDirection::HostToStorage:
        return "host_to_storage";
    }
    return "unknown";
}

std::string hicache_effect_patch_status_name(HiCacheEffectPatchStatus status) {
    switch (status) {
    case HiCacheEffectPatchStatus::Patchable:
        return "patchable";
    case HiCacheEffectPatchStatus::NotPatchable:
        return "not_patchable";
    case HiCacheEffectPatchStatus::Deferred:
        return "deferred";
    }
    return "unknown";
}

HiCacheEffectIntentCatalog HiCacheState::effect_intent_catalog() const {
    HiCacheEffectIntentCatalog catalog;
    catalog.kv_bytes_per_page = config_.kv_bytes_per_page;
    catalog.byte_projection_available = config_.kv_bytes_per_page > 0;
    catalog.byte_projection_source = catalog.byte_projection_available ? "target_config.kv_bytes_per_page" : "missing";

    std::vector<std::string> scope_names;
    scope_names.reserve(scopes_.size());
    for (const auto & scope_name : scopes_ | std::views::keys) scope_names.push_back(scope_name);
    std::ranges::sort(scope_names);

    for (const auto & scope_name : scope_names) {
        const auto & operations = scopes_.at(scope_name).async_ops;
        for (const auto & op : operations.prefetch_ops() | std::views::values) {
            auto intent = intent_from_header(op.header,
                                             HiCacheEffectType::Prefetch,
                                             HiCacheTransferDirection::StorageToHost,
                                             "host_storage_lane",
                                             op.completed_pages,
                                             static_cast<uint64_t>(op.planned_pages.size()),
                                             catalog);
            intent.state = prefetch_state_name(op.prefetch_state);
            catalog.intents.push_back(std::move(intent));
        }
        for (const auto & op : operations.loadback_ops() | std::views::values) {
            catalog.intents.push_back(intent_from_header(op.header,
                                                         HiCacheEffectType::Loadback,
                                                         HiCacheTransferDirection::HostToDevice,
                                                         "host_to_device_lane",
                                                         op.header.pages,
                                                         static_cast<uint64_t>(op.header.pages.size()),
                                                         catalog));
        }
        for (const auto & op : operations.storage_ops() | std::views::values) {
            // A storage operation is created only after the device value reaches host memory.
            catalog.intents.push_back(intent_from_header(op.header,
                                                         HiCacheEffectType::CommitDeviceToHost,
                                                         HiCacheTransferDirection::DeviceToHost,
                                                         "device_to_host_lane",
                                                         op.header.pages,
                                                         static_cast<uint64_t>(op.header.pages.size()),
                                                         catalog));
            catalog.intents.push_back(intent_from_header(op.header,
                                                         HiCacheEffectType::CommitHostToStorage,
                                                         HiCacheTransferDirection::HostToStorage,
                                                         "host_storage_lane",
                                                         op.header.pages,
                                                         static_cast<uint64_t>(op.header.pages.size()),
                                                         catalog));
        }
    }

    std::ranges::sort(catalog.intents, [](const auto & lhs, const auto & rhs) {
        if (lhs.logical_start_boundary.epoch != rhs.logical_start_boundary.epoch) return lhs.logical_start_boundary.epoch < rhs.logical_start_boundary.epoch;
        return lhs.effect_id < rhs.effect_id;
    });
    for (const auto & intent : catalog.intents) {
        if (!intent.not_patchable_reason.empty()) {
            auto & count = catalog.not_patchable_reasons[intent.not_patchable_reason];
            (void)core::checked_increment_u64(count, "HiCache not-patchable reason count exceeds uint64 range");
        }
    }
    return catalog;
}

} // namespace markov::trace_graph::modules::hicache::model
