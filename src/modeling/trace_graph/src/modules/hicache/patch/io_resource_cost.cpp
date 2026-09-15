/**
 * @file
 * @brief Interpretable per-effect service and control cost assembly.
 */
#include "io_resource_cost.hpp"

#include "markov/trace_graph/core/numeric.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace markov::trace_graph::modules::hicache::patch::io_resource_model_detail {

namespace {

std::optional<uint64_t> ceil_duration(double value) {
    if (!std::isfinite(value) || value < 0.0 || value > static_cast<double>(std::numeric_limits<uint64_t>::max())) return std::nullopt;
    return static_cast<uint64_t>(std::ceil(value));
}

uint64_t operation_count(const model::HiCacheEffectDecision & decision) {
    return decision.direction == model::HiCacheTransferDirection::None ? 0 : std::max<uint64_t>(1, static_cast<uint64_t>(decision.operation_ids.size()));
}

std::optional<uint64_t> project_service(HiCacheIoCostRecord & record, const model::HiCacheEffectDecision & decision,
                                      const frontend::HiCacheIoServiceModelConfig & service) {
    if (decision.effective_byte_count % decision.effective_page_count != 0) return std::nullopt;
    const auto page_bytes = decision.effective_byte_count / decision.effective_page_count;
    record.storage_existing_byte_count =
        core::checked_multiply_u64(decision.storage_existing_page_count, page_bytes, "HiCache existing-storage bytes exceed uint64 range");
    record.storage_new_byte_count = core::checked_multiply_u64(decision.storage_new_page_count, page_bytes, "HiCache new-storage bytes exceed uint64 range");
    uint64_t duration = 0;
    const auto add = [&](uint64_t pages, uint64_t calls, uint64_t existing) {
        const auto cost = hicache_service_cost(service, page_bytes, pages, calls, existing);
        if (!cost) return false;
        duration = core::checked_add_u64(duration, cost->duration_us, "I/O service duration overflow");
        record.calibration_setup_us += cost->setup_us;
        record.calibration_transfer_us += cost->transfer_us;
        record.storage_existing_service_us += cost->existing_us;
        record.storage_new_service_us += cost->new_us;
        record.bandwidth_bytes_per_sec = static_cast<uint64_t>(std::llround(cost->bandwidth_bytes_per_sec));
        return true;
    };
    const bool storage_write = service.direction == "host_to_storage";
    if (storage_write || service.direction == "storage_to_host") {
        uint64_t pages = 0, existing = 0, fresh = 0;
        for (const auto & batch : decision.storage_service_batches) {
            if (batch.operation_index >= record.operation_count || batch.page_count == 0
                || (storage_write && (batch.existing_page_count > batch.page_count
                                     || batch.new_page_count != batch.page_count - batch.existing_page_count))) return std::nullopt;
            pages = core::checked_add_u64(pages, batch.page_count, "Storage service pages overflow");
            existing = core::checked_add_u64(existing, batch.existing_page_count, "Storage existing pages overflow");
            fresh = core::checked_add_u64(fresh, batch.new_page_count, "Storage new pages overflow");
            if (!add(batch.page_count, 1, batch.existing_page_count)) return std::nullopt;
        }
        if (pages != decision.effective_page_count || existing != decision.storage_existing_page_count
            || fresh != decision.storage_new_page_count) return std::nullopt;
    }
    else if (!add(decision.effective_page_count, record.operation_count, 0)) return std::nullopt;
    const auto physical = record.calibration_setup_us + record.calibration_transfer_us;
    record.runtime_scale = storage_write && physical > 0.0
                               ? (record.storage_existing_service_us + record.storage_new_service_us) / physical
                               : service.runtime_scale;
    return duration;
}

} // namespace

HiCacheIoCostRecord cost_record(const model::HiCacheEffectDecision & decision, const model::HiCacheEffectDecisionLedger & ledger,
                                const frontend::HiCacheIoCostConfig & model_fields) {
    using model::HiCacheEffectType;
    using model::HiCacheTargetEffectState;
    using model::HiCacheTransferDirection;

    const auto kind = operation_kind(decision);
    const auto control = model_fields.control_models.find(kind);
    HiCacheIoCostRecord record{
        .effect_id = decision.effect_key,
        .effect_type = decision.effect_type,
        .direction = decision.direction,
        .target_effect_state = decision.target_effect_state,
        .operation_count = operation_count(decision),
        .effective_page_count = decision.effective_page_count,
        .effective_byte_count = decision.effective_byte_count,
        .storage_existing_page_count = decision.storage_existing_page_count,
        .storage_new_page_count = decision.storage_new_page_count,
        .storage_service_batches = decision.storage_service_batches,
        .operation_kind = kind,
        .host_control_page_count = control == model_fields.control_models.end() ? 0 : decision.effective_page_count,
        .host_control_operation_count = control == model_fields.control_models.end() ? 0 : operation_count(decision),
        .resource_scope = decision.cache_scope,
        .logical_order_epoch = decision.eligibility_boundary.epoch,
    };

    if (decision.zero_payload_prefetch()) {
        record.zero_payload_control = true;
        if (control == model_fields.control_models.end()) {
            record.status = HiCacheIoCostStatus::MissingHostControlModel;
            record.reason = "missing_prefetch_zero_payload_control";
            return record;
        }
        record.resource_lane = hicache_resource_lane(model_fields, kind, decision.cache_scope);
        if (decision.control_host_available_pages.size() != record.operation_count) {
            record.status = HiCacheIoCostStatus::MissingHostControlModel;
            record.reason = "missing_prefetch_terminal_host_state";
            return record;
        }
        for (const auto available : decision.control_host_available_pages) {
            if (!available) {
                record.status = HiCacheIoCostStatus::MissingHostControlModel;
                record.reason = "missing_prefetch_terminal_host_state";
                return record;
            }
        }
        const auto control_us = control->second.fixed_us_per_operation * static_cast<double>(record.operation_count);
        const auto duration = ceil_duration(control_us);
        if (!duration || *duration == 0) {
            record.status = HiCacheIoCostStatus::MissingHostControlModel;
            record.reason = "missing_prefetch_zero_payload_control";
            return record;
        }
        record.host_control_fixed_us = static_cast<double>(*duration);
        record.host_control_duration_us = *duration;
        record.status = HiCacheIoCostStatus::Ready;
        return record;
    }
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
        if (decision.effect_type == HiCacheEffectType::PrefetchVisibility || decision.effect_type == HiCacheEffectType::CommitCapacityGate)
            record.status = HiCacheIoCostStatus::Ready;
        else {
            record.status = HiCacheIoCostStatus::UnsupportedDirection;
            record.reason = "unsupported_transfer_direction";
        }
        return record;
    }
    if (decision.effective_page_count == 0 || decision.effective_byte_count == 0) {
        record.status = HiCacheIoCostStatus::MissingEffectiveBytes;
        record.reason = "missing_effective_payload";
        return record;
    }
    if (!ledger.byte_projection_available) {
        record.status = HiCacheIoCostStatus::MissingByteProjection;
        record.reason = "missing_byte_projection";
        return record;
    }
    if (record.resource_scope.empty()) {
        record.status = HiCacheIoCostStatus::MissingResourceScope;
        record.reason = "missing_resource_scope";
        return record;
    }
    const auto service_it = model_fields.service_models.find(kind);
    if (service_it == model_fields.service_models.end()) {
        record.status = HiCacheIoCostStatus::MissingBandwidth;
        record.reason = "missing_service_model";
        return record;
    }
    const auto & service = service_it->second;
    const auto duration = project_service(record, decision, service);
    if (!duration) {
        record.status = decision.effect_type == HiCacheEffectType::CommitHostToStorage ? HiCacheIoCostStatus::InconsistentStorageResidency
                                                                                       : HiCacheIoCostStatus::DurationOverflow;
        record.reason = "invalid_service_projection";
        return record;
    }
    record.duration_us = *duration;
    record.resource_lane = hicache_resource_lane(model_fields, kind, decision.cache_scope);

    if (kind == "prefetch" && (control == model_fields.control_models.end() || control->second.fixed_us_per_operation <= 0.0)) {
        record.status = HiCacheIoCostStatus::MissingHostControlModel;
        record.reason = "missing_prefetch_positive_payload_control";
        return record;
    }
    if (control != model_fields.control_models.end()) {
        record.host_control_fixed_us = control->second.fixed_us_per_operation * static_cast<double>(record.host_control_operation_count);
        const auto control_duration = ceil_duration(record.host_control_fixed_us);
        if (!control_duration) {
            record.status = HiCacheIoCostStatus::DurationOverflow;
            record.reason = "host_control_duration_overflow";
            return record;
        }
        record.host_control_duration_us = *control_duration;
    }
    record.status = HiCacheIoCostStatus::Ready;
    return record;
}

} // namespace markov::trace_graph::modules::hicache::patch::io_resource_model_detail
