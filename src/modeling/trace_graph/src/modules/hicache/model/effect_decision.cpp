/**
 * @file
 * @brief Builds the complete target-derived HiCache effect decision ledger.
 */
#include "markov/trace_graph/modules/hicache/model/state.hpp"

#include "markov/trace_graph/core/numeric.hpp"

#include <algorithm>
#include <limits>
#include <ranges>
#include <set>
#include <span>
#include <string_view>
#include <utility>

namespace markov::trace_graph::modules::hicache::model {

namespace effect_decision_detail {

struct EffectDescriptor {
    HiCacheEffectType type = HiCacheEffectType::Loadback;
    HiCacheTransferDirection direction = HiCacheTransferDirection::None;
    std::string_view resource_lane;
};

struct OperationEvidence {
    const HiCacheOperationHeader * header = nullptr;
    const std::vector<std::string> * effective_pages = nullptr;
    const std::vector<std::string> * storage_existing_pages = nullptr;
    const std::vector<std::string> * storage_new_pages = nullptr;
    std::string state;
    bool dependency_required = false;
};

std::vector<EffectDescriptor> descriptors_for_role(HiCacheFactRole role) {
    switch (role) {
    case HiCacheFactRole::CacheLookupInput:
        return {
            EffectDescriptor{
                             .type = HiCacheEffectType::Loadback,
                             .direction = HiCacheTransferDirection::HostToDevice,
                             .resource_lane = "host_to_device_lane",
                             },
            EffectDescriptor{
                             .type = HiCacheEffectType::CommitDeviceToHost,
                             .direction = HiCacheTransferDirection::DeviceToHost,
                             .resource_lane = "device_to_host_lane",
                             },
            EffectDescriptor{
                             .type = HiCacheEffectType::CommitHostToStorage,
                             .direction = HiCacheTransferDirection::HostToStorage,
                             .resource_lane = "host_storage_lane",
                             },
            EffectDescriptor{
                             .type = HiCacheEffectType::CommitCapacityGate,
                             .direction = HiCacheTransferDirection::None,
                             .resource_lane = {},
                             },
        };
    case HiCacheFactRole::PrefetchCandidateAnchor:
        return {
            EffectDescriptor{
                             .type = HiCacheEffectType::PrefetchIo,
                             .direction = HiCacheTransferDirection::StorageToHost,
                             .resource_lane = "host_storage_lane",
                             },
            EffectDescriptor{
                             .type = HiCacheEffectType::PrefetchVisibility,
                             .direction = HiCacheTransferDirection::None,
                             .resource_lane = {},
                             },
        };
    case HiCacheFactRole::CacheLifecycleCommit:
        return {
            EffectDescriptor{
                             .type = HiCacheEffectType::CommitDeviceToHost,
                             .direction = HiCacheTransferDirection::DeviceToHost,
                             .resource_lane = "device_to_host_lane",
                             },
            EffectDescriptor{
                             .type = HiCacheEffectType::CommitHostToStorage,
                             .direction = HiCacheTransferDirection::HostToStorage,
                             .resource_lane = "host_storage_lane",
                             },
            EffectDescriptor{
                             .type = HiCacheEffectType::CommitCapacityGate,
                             .direction = HiCacheTransferDirection::None,
                             .resource_lane = {},
                             },
        };
    case HiCacheFactRole::CacheExtendInput:
        return {
            EffectDescriptor{
                             .type = HiCacheEffectType::CommitDeviceToHost,
                             .direction = HiCacheTransferDirection::DeviceToHost,
                             .resource_lane = "device_to_host_lane",
                             },
            EffectDescriptor{
                             .type = HiCacheEffectType::CommitHostToStorage,
                             .direction = HiCacheTransferDirection::HostToStorage,
                             .resource_lane = "host_storage_lane",
                             },
            EffectDescriptor{
                             .type = HiCacheEffectType::CommitCapacityGate,
                             .direction = HiCacheTransferDirection::None,
                             .resource_lane = {},
                             },
        };
    case HiCacheFactRole::Unknown:
        return {};
    }
    return {};
}

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

std::string request_identity(const HiCacheFact & fact, uint64_t role_ordinal) {
    if (fact.full_path_span.valid && !fact.full_path_span.path_id.empty()) {
        return fact.full_path_span.path_id + ":" + std::to_string(fact.full_path_span.begin) + ":" + std::to_string(fact.full_path_span.end);
    }
    return "missing_token_range:" + fact.role + ":" + std::to_string(role_ordinal);
}

std::string opportunity_key(const EffectDescriptor & descriptor, const std::string & cache_scope, const std::string & identity,
                            const std::string & source_fact_role, uint64_t source_fact_ordinal, uint64_t decision_ordinal) {
    return hicache_effect_type_name(descriptor.type) + "|" + cache_scope + "|" + identity + "|" + source_fact_role + "|" + std::to_string(source_fact_ordinal)
           + "|" + std::to_string(decision_ordinal);
}

std::string effect_family_key(const std::string & cache_scope, const std::string & identity, const std::string & source_fact_role,
                              uint64_t source_fact_ordinal) {
    return cache_scope + "|" + identity + "|" + source_fact_role + "|" + std::to_string(source_fact_ordinal);
}

std::vector<HiCacheEffectSegment> effect_segments(const HiCacheFact & fact, const HiCachePagePath & page_path, HiCacheTransferDirection direction) {
    std::vector<HiCacheEffectSegment> segments;
    segments.reserve(page_path.pages.size());
    for (const auto & page : page_path.pages) {
        const auto token_begin = core::checked_add_u64(fact.full_path_span.begin, page.token_begin, "HiCache effect token begin exceeds uint64 range");
        const auto token_end = core::checked_add_u64(fact.full_path_span.begin, page.token_end, "HiCache effect token end exceeds uint64 range");
        const auto range_key = fact.full_path_span.path_id + ":" + std::to_string(token_begin) + ":" + std::to_string(token_end);
        segments.push_back(HiCacheEffectSegment{
            .segment_key = range_key + ":" + hicache_transfer_direction_name(direction),
            .token_path_id = fact.full_path_span.path_id,
            .token_begin = token_begin,
            .token_end = token_end,
            .target_page_id = page.id,
        });
    }
    return segments;
}

std::vector<HiCacheEffectSegment> effective_segments(const HiCacheEffectOpportunity & opportunity, const std::vector<std::string> & pages) {
    const std::set<std::string> selected(pages.begin(), pages.end());
    std::vector<HiCacheEffectSegment> segments;
    for (const auto & segment : opportunity.candidate_segments) {
        if (selected.contains(segment.target_page_id)) segments.push_back(segment);
    }
    return segments;
}

std::string page_identity(std::string_view page_id) {
    const auto separator = page_id.rfind('|');
    return separator == std::string_view::npos ? std::string(page_id) : std::string(page_id.substr(separator + 1));
}

uint64_t opportunity_page_size(const HiCacheEffectOpportunity & opportunity) {
    if (opportunity.candidate_segments.empty()) return 0;
    const auto & first = opportunity.candidate_segments.front();
    return first.token_end >= first.token_begin ? first.token_end - first.token_begin : 0;
}

std::vector<HiCacheEffectSegment> operation_page_segments(const HiCacheEffectOpportunity & opportunity, const std::vector<std::string> & pages) {
    const auto page_size = opportunity_page_size(opportunity);
    if (page_size == 0) return {};
    std::vector<HiCacheEffectSegment> segments;
    segments.reserve(pages.size());
    for (const auto & page : pages) {
        const auto identity = page_identity(page);
        segments.push_back(HiCacheEffectSegment{
            .segment_key = "operation_page:" + identity + ":" + hicache_transfer_direction_name(opportunity.direction),
            .token_path_id = identity,
            .token_begin = 0,
            .token_end = page_size,
            .target_page_id = page,
        });
    }
    return segments;
}

std::vector<std::string> unique_pages(const std::vector<OperationEvidence> & evidence) {
    std::vector<std::string> pages;
    std::set<std::string> seen;
    for (const auto & operation : evidence) {
        if (operation.effective_pages == nullptr) continue;
        for (const auto & page : *operation.effective_pages) {
            if (seen.insert(page).second) pages.push_back(page);
        }
    }
    return pages;
}

bool operation_is_pending(const HiCacheOperationHeader & header) {
    return header.state == HiCacheOperationState::Created || header.state == HiCacheOperationState::Queued || header.state == HiCacheOperationState::Ready;
}

HiCacheTargetEffectState target_state(const HiCacheEffectOpportunity & opportunity, const std::vector<OperationEvidence> & evidence,
                                      uint64_t effective_page_count, uint64_t candidate_page_count) {
    if (!opportunity.input_ready) return HiCacheTargetEffectState::Unresolved;
    if (evidence.empty()) return HiCacheTargetEffectState::NotRequired;
    if (opportunity.effect_type == HiCacheEffectType::PrefetchVisibility || opportunity.effect_type == HiCacheEffectType::CommitCapacityGate) {
        const bool required = std::ranges::any_of(evidence, [](const auto & operation) { return operation.dependency_required; });
        if (!required) return HiCacheTargetEffectState::NotRequired;
        const bool consumer_available = std::ranges::any_of(evidence, [](const auto & operation) {
            return operation.header != nullptr && operation.header->consumer_epoch > 0 && operation.header->consumer_source_available;
        });
        return consumer_available ? HiCacheTargetEffectState::Required : HiCacheTargetEffectState::Deferred;
    }
    if (effective_page_count == 0) {
        if (std::ranges::any_of(evidence, [](const auto & operation) { return operation.header != nullptr && operation_is_pending(*operation.header); }))
            return HiCacheTargetEffectState::Deferred;
        return HiCacheTargetEffectState::NotRequired;
    }
    if (effective_page_count < candidate_page_count) return HiCacheTargetEffectState::Partial;
    return HiCacheTargetEffectState::Required;
}

HiCacheEffectBoundary completion_boundary(const HiCacheEffectOpportunity & opportunity, const std::vector<OperationEvidence> & evidence) {
    const HiCacheOperationHeader * latest = nullptr;
    for (const auto & operation : evidence) {
        if (operation.header == nullptr) continue;
        if (latest == nullptr || operation.header->complete_epoch > latest->complete_epoch
            || (operation.header->complete_epoch == latest->complete_epoch && operation.header->boundary_epoch > latest->boundary_epoch))
            latest = operation.header;
    }
    if (latest == nullptr) {
        return HiCacheEffectBoundary{
            .kind = "target_decision_noop",
            .epoch = opportunity.eligibility_boundary.epoch,
            .timestamp_us = opportunity.eligibility_boundary.timestamp_us,
        };
    }
    if (latest->complete_epoch > 0) {
        return HiCacheEffectBoundary{
            .kind = "target_operation_completion",
            .epoch = latest->complete_epoch,
            .timestamp_us = latest->complete_ts,
        };
    }
    if (latest->boundary_epoch > 0) {
        return HiCacheEffectBoundary{
            .kind = "target_operation_boundary",
            .epoch = latest->boundary_epoch,
            .timestamp_us = latest->boundary_ts,
        };
    }
    return HiCacheEffectBoundary{
        .kind = "target_operation_pending",
        .epoch = opportunity.eligibility_boundary.epoch,
        .timestamp_us = opportunity.eligibility_boundary.timestamp_us,
    };
}

HiCacheEffectBoundary consumer_boundary(HiCacheEffectType type, const std::vector<OperationEvidence> & evidence, const HiCacheEffectBoundary & completion) {
    auto boundary = completion;
    const HiCacheOperationHeader * latest_consumer = nullptr;
    for (const auto & operation : evidence) {
        if (operation.header == nullptr || operation.header->consumer_epoch == 0) continue;
        if (latest_consumer == nullptr || operation.header->consumer_epoch > latest_consumer->consumer_epoch) latest_consumer = operation.header;
    }
    if (latest_consumer != nullptr) {
        boundary.epoch = latest_consumer->consumer_epoch;
        boundary.timestamp_us = latest_consumer->consumer_ts;
        boundary.source_fact_role = latest_consumer->consumer_source_fact_role;
        if (latest_consumer->consumer_source_available) {
            boundary.source_node_id = latest_consumer->consumer_source_node_id;
            boundary.execution_anchor_node_id = latest_consumer->consumer_execution_anchor_node_id;
            boundary.source_event_index = latest_consumer->consumer_source_event_index;
        }
    }
    switch (type) {
    case HiCacheEffectType::Loadback:
        boundary.kind = "cache_lookup_consumer";
        break;
    case HiCacheEffectType::PrefetchIo:
        boundary.kind = "prefetch_visibility_consumer";
        break;
    case HiCacheEffectType::PrefetchVisibility:
        boundary.kind = latest_consumer == nullptr ? "cache_extend_consumer_unresolved" : "cache_extend_consumer";
        break;
    case HiCacheEffectType::CommitDeviceToHost:
        boundary.kind = "host_storage_write_consumer";
        break;
    case HiCacheEffectType::CommitHostToStorage:
        boundary.kind = "storage_ack_consumer";
        break;
    case HiCacheEffectType::CommitCapacityGate:
        boundary.kind = latest_consumer == nullptr ? "host_capacity_consumer_unresolved" : "host_capacity_consumer";
        break;
    }
    return boundary;
}

std::string decision_reason(HiCacheTargetEffectState state) {
    switch (state) {
    case HiCacheTargetEffectState::Required:
        return "target state materialized the complete direct effect";
    case HiCacheTargetEffectState::NotRequired:
        return "target state explicitly completed this opportunity without a direct effect";
    case HiCacheTargetEffectState::Partial:
        return "target state materialized only a token-range subset of the opportunity";
    case HiCacheTargetEffectState::Deferred:
        return "target operation remains pending beyond the current control boundary";
    case HiCacheTargetEffectState::Unresolved:
        return "target decision lacks a complete canonical token-range input";
    }
    return "unknown target decision";
}

HiCacheScheduleSensitivity schedule_sensitivity(HiCacheEffectType type) {
    switch (type) {
    case HiCacheEffectType::Loadback:
    case HiCacheEffectType::PrefetchVisibility:
        return HiCacheScheduleSensitivity::ArrivalScheduleSensitive;
    case HiCacheEffectType::PrefetchIo:
    case HiCacheEffectType::CommitDeviceToHost:
    case HiCacheEffectType::CommitHostToStorage:
    case HiCacheEffectType::CommitCapacityGate:
        return HiCacheScheduleSensitivity::ScheduleInvariant;
    }
    return HiCacheScheduleSensitivity::ScheduleInvariant;
}

void classify_patch_status(HiCacheEffectDecision & decision, const HiCacheEffectDecisionLedger & ledger) {
    if (decision.target_effect_state == HiCacheTargetEffectState::Deferred || decision.target_effect_state == HiCacheTargetEffectState::Unresolved) {
        decision.patch_status = HiCacheEffectPatchStatus::Deferred;
        decision.not_patchable_reason =
            decision.target_effect_state == HiCacheTargetEffectState::Deferred ? "target_effect_deferred" : "target_effect_unresolved";
        return;
    }
    decision.patch_status = HiCacheEffectPatchStatus::NotPatchable;
    if (decision.direction != HiCacheTransferDirection::None && decision.effective_page_count > 0 && !ledger.byte_projection_available)
        decision.not_patchable_reason = "missing_byte_projection";
    else if (decision.direction != HiCacheTransferDirection::None && decision.effective_page_count > 0 && decision.effective_byte_count == 0)
        decision.not_patchable_reason = "byte_projection_overflow";
    else decision.not_patchable_reason = "source_attribution_not_evaluated";
}

template <typename OperationMap, typename Matcher, typename Projector>
void append_operation_evidence(std::vector<OperationEvidence> & evidence, const OperationMap & operations, const HiCacheEffectOpportunity & opportunity,
                               Matcher matcher, Projector projector) {
    for (const auto & operation : operations | std::views::values) {
        if (!matcher(operation.header, opportunity)) continue;
        evidence.push_back(projector(operation));
    }
}

bool exact_source_fact_match(const HiCacheOperationHeader & header, const HiCacheEffectOpportunity & opportunity) {
    if (header.source_event_index != opportunity.source_event_index) return false;
    if (!header.request_id.empty() && !opportunity.request_id_provenance.empty()) return header.request_id == opportunity.request_id_provenance;
    return true;
}

bool commit_operation_match(const HiCacheOperationHeader & header, const HiCacheEffectOpportunity & opportunity,
                            std::span<const HiCacheEffectOpportunity> opportunities) {
    if (header.cache_scope != opportunity.state_scope_key) return false;
    const bool has_exact_opportunity = std::ranges::any_of(opportunities, [&](const auto & candidate) {
        return candidate.effect_type == opportunity.effect_type && candidate.state_scope_key == header.cache_scope
               && exact_source_fact_match(header, candidate);
    });
    if (has_exact_opportunity) return exact_source_fact_match(header, opportunity);
    if (!header.request_id.empty() && !opportunity.request_id_provenance.empty()) return header.request_id == opportunity.request_id_provenance;
    if (header.enqueue_ts > opportunity.eligibility_boundary.timestamp_us) return false;

    uint64_t earliest_lifecycle_ts = std::numeric_limits<uint64_t>::max();
    std::set<std::string_view> earliest_families;
    for (const auto & candidate : opportunities) {
        if (candidate.effect_type != HiCacheEffectType::CommitDeviceToHost || candidate.state_scope_key != header.cache_scope
            || candidate.eligibility_boundary.timestamp_us < header.enqueue_ts)
            continue;
        const auto timestamp = candidate.eligibility_boundary.timestamp_us;
        if (timestamp < earliest_lifecycle_ts) {
            earliest_lifecycle_ts = timestamp;
            earliest_families.clear();
        }
        if (timestamp == earliest_lifecycle_ts) earliest_families.insert(candidate.effect_family_key);
    }
    return earliest_lifecycle_ts == opportunity.eligibility_boundary.timestamp_us && earliest_families.size() == 1
           && earliest_families.contains(opportunity.effect_family_key);
}

std::vector<OperationEvidence> operation_evidence(const HiCacheEffectOpportunity & opportunity, const HiCacheAsyncOperationTable & operations,
                                                  std::span<const HiCacheEffectOpportunity> opportunities) {
    std::vector<OperationEvidence> evidence;
    switch (opportunity.effect_type) {
    case HiCacheEffectType::Loadback:
        append_operation_evidence(evidence, operations.loadback_ops(), opportunity, exact_source_fact_match, [](const auto & operation) {
            return OperationEvidence{
                .header = &operation.header,
                .effective_pages = &operation.header.pages,
                .state = operation_state_name(operation.header.state),
            };
        });
        break;
    case HiCacheEffectType::PrefetchIo:
        append_operation_evidence(evidence, operations.prefetch_ops(), opportunity, exact_source_fact_match, [](const auto & operation) {
            return OperationEvidence{
                .header = &operation.header,
                .effective_pages = operation.payload_transfer_issued ? &operation.hit_pages : nullptr,
                .state = prefetch_state_name(operation.prefetch_state),
            };
        });
        break;
    case HiCacheEffectType::PrefetchVisibility:
        append_operation_evidence(evidence, operations.prefetch_ops(), opportunity, exact_source_fact_match, [](const auto & operation) {
            return OperationEvidence{
                .header = &operation.header,
                .effective_pages = &operation.completed_pages,
                .state = prefetch_state_name(operation.prefetch_state),
                .dependency_required = operation.visibility_dependency_required,
            };
        });
        break;
    case HiCacheEffectType::CommitDeviceToHost:
        append_operation_evidence(
            evidence,
            operations.storage_ops(),
            opportunity,
            [&](const auto & header, const auto & candidate) { return commit_operation_match(header, candidate, opportunities); },
            [](const auto & operation) {
                return OperationEvidence{
                    .header = &operation.header,
                    .effective_pages = &operation.device_to_host_pages,
                    .state = operation_state_name(operation.header.state),
                };
            });
        break;
    case HiCacheEffectType::CommitHostToStorage:
        append_operation_evidence(
            evidence,
            operations.storage_ops(),
            opportunity,
            [&](const auto & header, const auto & candidate) { return commit_operation_match(header, candidate, opportunities); },
            [](const auto & operation) {
                return OperationEvidence{
                    .header = &operation.header,
                    .effective_pages = &operation.host_to_storage_pages,
                    .storage_existing_pages = &operation.host_to_storage_existing_pages,
                    .storage_new_pages = &operation.host_to_storage_new_pages,
                    .state = operation_state_name(operation.header.state),
                };
            });
        break;
    case HiCacheEffectType::CommitCapacityGate:
        append_operation_evidence(
            evidence,
            operations.storage_ops(),
            opportunity,
            [&](const auto & header, const auto & candidate) { return commit_operation_match(header, candidate, opportunities); },
            [](const auto & operation) {
                return OperationEvidence{
                    .header = &operation.header,
                    .effective_pages = &operation.capacity_gate_pages,
                    .state = operation_state_name(operation.header.state),
                    .dependency_required = false,
                };
            });
        break;
    }
    std::ranges::sort(evidence, [](const auto & left, const auto & right) {
        return left.header != nullptr && right.header != nullptr && left.header->operation_id < right.header->operation_id;
    });
    return evidence;
}

HiCacheEffectDecision build_decision(const HiCacheEffectOpportunity & opportunity, const HiCacheAsyncOperationTable * operations,
                                     std::span<const HiCacheEffectOpportunity> opportunities, const HiCacheEffectDecisionLedger & ledger) {
    const auto evidence = operations == nullptr ? std::vector<OperationEvidence>{} : operation_evidence(opportunity, *operations, opportunities);
    auto pages = unique_pages(evidence);
    const bool operation_shaped = opportunity.effect_type == HiCacheEffectType::CommitDeviceToHost
                                  || opportunity.effect_type == HiCacheEffectType::CommitHostToStorage
                                  || opportunity.effect_type == HiCacheEffectType::CommitCapacityGate;
    auto segments = operation_shaped ? operation_page_segments(opportunity, pages) : effective_segments(opportunity, pages);
    auto candidate_segments = operation_shaped ? segments : opportunity.candidate_segments;
    auto state = target_state(opportunity, evidence, static_cast<uint64_t>(segments.size()), static_cast<uint64_t>(candidate_segments.size()));
    auto completion = completion_boundary(opportunity, evidence);
    HiCacheEffectDecision decision{
        .effect_key = opportunity.opportunity_key,
        .opportunity_key = opportunity.opportunity_key,
        .effect_family_key = opportunity.effect_family_key,
        .effect_type = opportunity.effect_type,
        .direction = opportunity.direction,
        .cache_scope = opportunity.cache_scope,
        .request_id_provenance = opportunity.request_id_provenance,
        .source_fact_role = opportunity.source_fact_role,
        .source_fact_ordinal = opportunity.source_fact_ordinal,
        .source_node_id = opportunity.source_node_id,
        .source_execution_anchor_node_id = opportunity.source_execution_anchor_node_id,
        .candidate_segments = std::move(candidate_segments),
        .effective_segments = std::move(segments),
        .effective_pages = std::move(pages),
        .eligibility_boundary = opportunity.eligibility_boundary,
        .consumer_boundary = consumer_boundary(opportunity.effect_type, evidence, completion),
        .resource_lane = opportunity.resource_lane,
        .target_effect_state = state,
        .schedule_sensitivity = schedule_sensitivity(opportunity.effect_type),
        .reason = decision_reason(state),
    };
    for (const auto & operation : evidence) {
        if (operation.header != nullptr) decision.operation_ids.push_back(operation.header->operation_id);
        if (decision.state.empty()) decision.state = operation.state;
        else if (decision.state != operation.state) decision.state = "mixed";
    }
    if (decision.state.empty()) decision.state = hicache_target_effect_state_name(state);
    decision.effective_page_count = static_cast<uint64_t>(decision.effective_segments.size());
    if (decision.direction != HiCacheTransferDirection::None)
        decision.effective_byte_count = projected_bytes(decision.effective_page_count, ledger.kv_bytes_per_page);
    if (decision.effect_type == HiCacheEffectType::CommitHostToStorage && !decision.effective_pages.empty()) {
        const std::set<std::string> effective_page_set(decision.effective_pages.begin(), decision.effective_pages.end());
        std::set<std::string> existing_pages;
        std::set<std::string> new_pages;
        for (const auto & operation : evidence) {
            if (operation.storage_existing_pages != nullptr) {
                for (const auto & page : *operation.storage_existing_pages)
                    if (effective_page_set.contains(page)) existing_pages.insert(page);
            }
            if (operation.storage_new_pages != nullptr) {
                for (const auto & page : *operation.storage_new_pages)
                    if (effective_page_set.contains(page)) new_pages.insert(page);
            }
        }
        decision.storage_existing_page_count = static_cast<uint64_t>(existing_pages.size());
        decision.storage_new_page_count = static_cast<uint64_t>(new_pages.size());
        const auto retained_existing_operation_counts = [&] {
            std::vector<uint64_t> counts;
            counts.reserve(evidence.size());
            uint64_t total = 0;
            for (const auto & operation : evidence) {
                if (operation.header == nullptr) return std::vector<uint64_t>{};
                std::set<std::string> operation_pages;
                if (operation.storage_existing_pages != nullptr) {
                    for (const auto & page : *operation.storage_existing_pages)
                        if (effective_page_set.contains(page)) operation_pages.insert(page);
                }
                const auto count = static_cast<uint64_t>(operation_pages.size());
                total = core::checked_add_u64(total, count, "H2S per-operation page count exceeds uint64 range");
                counts.push_back(count);
            }
            if (counts.size() != decision.operation_ids.size() || total != decision.storage_existing_page_count) return std::vector<uint64_t>{};
            return counts;
        };
        decision.storage_existing_operation_page_counts = retained_existing_operation_counts();
    }
    classify_patch_status(decision, ledger);
    return decision;
}

} // namespace effect_decision_detail

using effect_decision_detail::build_decision;
using effect_decision_detail::descriptors_for_role;
using effect_decision_detail::effect_family_key;
using effect_decision_detail::effect_segments;
using effect_decision_detail::opportunity_key;
using effect_decision_detail::request_identity;

void HiCacheState::observe_effect_opportunities(const HiCacheFact & fact, HiCacheFactRole role) {
    const auto descriptors = descriptors_for_role(role);
    if (descriptors.empty()) return;

    const auto page_size = pager_.page_size_for_fact(fact);
    auto append_opportunities = [&](const HiCacheFact & opportunity_fact, const HiCacheTokenResolution & resolution) {
        const auto page_path = page_path_from_resolution(opportunity_fact, resolution);
        const auto state_scope_key = normalized_scope(opportunity_fact);
        auto scope_identity = effect_scope_identities_.find(state_scope_key);
        if (scope_identity == effect_scope_identities_.end()) {
            const auto scope_ordinal = core::checked_increment_u64(effect_scope_epoch_, "HiCache effect scope ordinal exceeds uint64 range");
            scope_identity = effect_scope_identities_.emplace(state_scope_key, "scope:" + std::to_string(scope_ordinal)).first;
        }
        const auto & scope = scope_identity->second;
        auto & role_ordinal_counter = effect_fact_ordinals_[scope + "|" + opportunity_fact.role];
        const auto role_ordinal = core::checked_increment_u64(role_ordinal_counter, "HiCache effect fact ordinal exceeds uint64 range");
        const auto opportunity_epoch = core::checked_increment_u64(effect_opportunity_epoch_, "HiCache effect opportunity epoch exceeds uint64 range");
        const auto identity = request_identity(opportunity_fact, role_ordinal);

        uint64_t decision_ordinal = 0;
        for (const auto & descriptor : descriptors) {
            effect_opportunities_.push_back(HiCacheEffectOpportunity{
                .opportunity_key = opportunity_key(descriptor, scope, identity, opportunity_fact.role, role_ordinal, decision_ordinal),
                .effect_family_key = effect_family_key(scope, identity, opportunity_fact.role, role_ordinal),
                .effect_type = descriptor.type,
                .direction = descriptor.direction,
                .cache_scope = scope,
                .state_scope_key = state_scope_key,
                .request_identity = identity,
                .request_id_provenance = opportunity_fact.request_id,
                .source_fact_role = opportunity_fact.role,
                .source_fact_ordinal = role_ordinal,
                .decision_ordinal = decision_ordinal,
                .source_fact_seq_no = opportunity_fact.seq_no,
                .source_node_id = opportunity_fact.source_node_id,
                .source_execution_anchor_node_id = opportunity_fact.execution_anchor_node_id,
                .source_event_index = opportunity_fact.source_event_index,
                .input_ready = resolution.ok() && opportunity_fact.full_path_span.valid,
                .eligibility_boundary =
                    HiCacheEffectBoundary{
                                          .kind = "source_fact_eligibility",
                                          .epoch = opportunity_epoch,
                                          .timestamp_us = opportunity_fact.ts,
                                          },
                .resource_lane = std::string(descriptor.resource_lane),
                .candidate_segments = effect_segments(opportunity_fact, page_path, descriptor.direction),
            });
            decision_ordinal = core::checked_add_u64(decision_ordinal, 1, "HiCache decision ordinal exceeds uint64 range");
        }
    };

    switch (role) {
    case HiCacheFactRole::CacheLookupInput: {
        const auto resolution = token_directory_.resolve_cache_lookup_path(fact, page_size);
        append_opportunities(fact, resolution);
        break;
    }
    case HiCacheFactRole::PrefetchCandidateAnchor: {
        const auto resolution = token_directory_.resolve_prefetch_candidate_path(fact, page_size);
        append_opportunities(fact, resolution);
        break;
    }
    case HiCacheFactRole::CacheLifecycleCommit: {
        const auto resolution = token_directory_.resolve_cache_lifecycle_commit_path(fact, page_size);
        append_opportunities(fact, resolution);
        break;
    }
    case HiCacheFactRole::CacheExtendInput: {
        const auto batch_resolution = token_directory_.resolve_cache_extend_paths(fact, page_size);
        for (size_t index = 0; index < fact.batch_paths.size(); ++index) {
            HiCacheFact entry_fact = fact;
            const auto & entry = fact.batch_paths[index];
            entry_fact.request_id = entry.request_id;
            entry_fact.full_path_span = entry.full_path_span;
            entry_fact.full_path_tokens = entry.full_path_tokens;
            entry_fact.token_count = entry.token_count;
            const auto resolution = index < batch_resolution.entries.size() ? batch_resolution.entries[index] : HiCacheTokenResolution{};
            append_opportunities(entry_fact, resolution);
        }
        break;
    }
    case HiCacheFactRole::Unknown:
        return;
    }
}

HiCacheEffectDecisionLedger HiCacheState::effect_decision_ledger() const {
    HiCacheEffectDecisionLedger ledger;
    ledger.kv_bytes_per_page = config_.kv_bytes_per_page;
    ledger.l2_capacity_pages = config_.l2_capacity_pages;
    ledger.l2_capacity_bytes =
        core::checked_multiply_u64(config_.l2_capacity_pages, config_.kv_bytes_per_page, "HiCache target L2 capacity bytes exceed uint64 range");
    ledger.byte_projection_available = config_.kv_bytes_per_page > 0;
    ledger.byte_projection_source = ledger.byte_projection_available ? "target_config.kv_bytes_per_page" : "missing";
    ledger.decisions.reserve(effect_opportunities_.size());

    for (const auto & opportunity : effect_opportunities_) {
        const auto scope = scopes_.find(opportunity.state_scope_key);
        const auto * operations = scope == scopes_.end() ? nullptr : &scope->second.async_ops;
        ledger.decisions.push_back(build_decision(opportunity, operations, effect_opportunities_, ledger));
    }
    std::ranges::sort(ledger.decisions, [](const auto & left, const auto & right) {
        if (left.eligibility_boundary.epoch != right.eligibility_boundary.epoch) return left.eligibility_boundary.epoch < right.eligibility_boundary.epoch;
        return left.effect_key < right.effect_key;
    });
    for (const auto & decision : ledger.decisions) {
        if (!decision.not_patchable_reason.empty()) {
            auto & count = ledger.not_patchable_reasons[decision.not_patchable_reason];
            (void)core::checked_increment_u64(count, "HiCache not-patchable reason count exceeds uint64 range");
        }
    }
    if (ledger.unresolved_count() > 0 || !ledger.missing_facts.empty()) ledger.status = "partial";
    return ledger;
}

} // namespace markov::trace_graph::modules::hicache::model
