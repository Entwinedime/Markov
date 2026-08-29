/**
 * @file
 * @brief Interpretable per-effect service and control cost assembly.
 */
#include "io_resource_cost.hpp"

#include "markov/trace_graph/core/numeric.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <optional>
#include <ranges>

namespace markov::trace_graph::modules::hicache::patch::io_resource_model_detail {

namespace {

std::optional<uint64_t> ceil_duration(double value) {
    if (!std::isfinite(value) || value < 0.0 || value > static_cast<double>(std::numeric_limits<uint64_t>::max())) return std::nullopt;
    return static_cast<uint64_t>(std::ceil(value));
}

uint64_t operation_count(const model::HiCacheEffectDecision & decision) {
    return decision.direction == model::HiCacheTransferDirection::None
               ? 0
               : std::max<uint64_t>(1, static_cast<uint64_t>(decision.operation_ids.size()));
}

bool has_host_control(const frontend::HiCacheIoControlModelConfig & control) {
    return control.fixed_us_per_operation > 0.0 || control.zero_payload_fixed_us_per_operation > 0.0 || control.per_page_us > 0.0;
}

bool zero_payload_prefetch(const model::HiCacheEffectDecision & decision) {
    return decision.effect_type == model::HiCacheEffectType::PrefetchIo
           && decision.target_effect_state == model::HiCacheTargetEffectState::NotRequired && decision.state == "applied"
           && !decision.operation_ids.empty() && decision.effective_page_count == 0 && decision.effective_byte_count == 0;
}

std::optional<double> project_h2s(HiCacheIoCostRecord & record, const model::HiCacheEffectDecision & decision,
                                  const frontend::HiCacheIoServiceModelConfig & service) {
    if (decision.effective_byte_count % decision.effective_page_count != 0) return std::nullopt;
    const auto page_bytes = decision.effective_byte_count / decision.effective_page_count;
    record.storage_existing_byte_count = core::checked_multiply_u64(
        decision.storage_existing_page_count, page_bytes, "HiCache existing-storage bytes exceed uint64 range");
    record.storage_new_byte_count = core::checked_multiply_u64(
        decision.storage_new_page_count, page_bytes, "HiCache new-storage bytes exceed uint64 range");
    if (core::checked_add_u64(record.storage_existing_byte_count, record.storage_new_byte_count,
                              "HiCache split storage bytes exceed uint64 range")
        != decision.effective_byte_count)
        return std::nullopt;

    double existing_calibration = 0.0;
    const auto & shapes = decision.storage_existing_operation_page_counts;
    const bool exact_shapes = shapes.size() == record.operation_count
                              && std::accumulate(shapes.begin(), shapes.end(), uint64_t{ 0 }) == decision.storage_existing_page_count;
    if (decision.storage_existing_page_count > 0 && exact_shapes) {
        for (const auto pages : shapes) {
            if (pages == 0) continue;
            const auto bandwidth = interpolated_existing_key_bandwidth(service, static_cast<double>(page_bytes), static_cast<double>(pages));
            if (!std::isfinite(bandwidth) || bandwidth <= 0.0) return std::nullopt;
            existing_calibration += static_cast<double>(pages) * static_cast<double>(page_bytes) * 1'000'000.0 / bandwidth;
        }
    }
    else if (decision.storage_existing_page_count > 0) {
        const auto average_pages = static_cast<double>(decision.storage_existing_page_count) / static_cast<double>(record.operation_count);
        const auto bandwidth = interpolated_existing_key_bandwidth(service, static_cast<double>(page_bytes), average_pages);
        if (!std::isfinite(bandwidth) || bandwidth <= 0.0) return std::nullopt;
        existing_calibration = static_cast<double>(record.storage_existing_byte_count) * 1'000'000.0 / bandwidth;
    }

    double new_calibration = 0.0;
    if (decision.storage_new_page_count > 0) {
        const auto parameters = interpolated_new_write(service, static_cast<double>(page_bytes));
        if (!std::isfinite(parameters.bandwidth_bytes_per_sec) || parameters.bandwidth_bytes_per_sec <= 0.0) return std::nullopt;
        auto new_operations = record.operation_count;
        if (exact_shapes) {
            const auto operations_without_existing = static_cast<uint64_t>(std::ranges::count(shapes, uint64_t{ 0 }));
            if (operations_without_existing > 0) new_operations = operations_without_existing;
        }
        const auto setup = static_cast<double>(new_operations) * parameters.setup_us_per_operation;
        const auto transfer = static_cast<double>(record.storage_new_byte_count) * 1'000'000.0 / parameters.bandwidth_bytes_per_sec;
        record.calibration_setup_us += setup;
        record.calibration_transfer_us += transfer;
        new_calibration = setup + transfer;
    }
    record.storage_existing_service_us = existing_calibration * service.existing_runtime_scale;
    record.storage_new_service_us = new_calibration * service.new_runtime_scale;
    return record.storage_existing_service_us + record.storage_new_service_us;
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
        .storage_existing_operation_page_counts = decision.storage_existing_operation_page_counts,
        .operation_kind = kind,
        .host_control_page_count = control == model_fields.control_models.end() ? 0 : decision.effective_page_count,
        .host_control_operation_count = control == model_fields.control_models.end() ? 0 : operation_count(decision),
        .resource_scope = decision.cache_scope,
        .logical_order_epoch = decision.eligibility_boundary.epoch,
    };

    if (control != model_fields.control_models.end() && zero_payload_prefetch(decision)) {
        record.zero_payload_control = true;
        record.resource_lane = resource_lane(decision, model_fields);
        const auto duration = ceil_duration(control->second.zero_payload_fixed_us_per_operation * record.operation_count);
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
    const auto page_bytes = static_cast<double>(decision.effective_byte_count) / static_cast<double>(decision.effective_page_count);
    std::optional<double> projected;
    if (decision.effect_type == HiCacheEffectType::CommitHostToStorage) {
        if (core::checked_add_u64(decision.storage_existing_page_count, decision.storage_new_page_count,
                                  "HiCache storage residency pages exceed uint64 range")
            != decision.effective_page_count)
            projected = std::nullopt;
        else projected = project_h2s(record, decision, service);
    }
    else {
        double bandwidth = service.bandwidth_bytes_per_sec;
        if (!service.page_bandwidth_points.empty()) bandwidth = interpolated_page_bandwidth(service.page_bandwidth_points, page_bytes);
        if (!std::isfinite(bandwidth) || bandwidth <= 0.0 || !std::isfinite(service.runtime_scale) || service.runtime_scale <= 0.0) {
            record.status = HiCacheIoCostStatus::MissingBandwidth;
            record.reason = "invalid_service_coefficients";
            return record;
        }
        record.bandwidth_bytes_per_sec = static_cast<uint64_t>(std::llround(bandwidth / service.runtime_scale));
        record.calibration_setup_us = service.setup_us_per_operation * static_cast<double>(record.operation_count)
                                      + service.setup_us_per_page * static_cast<double>(record.effective_page_count);
        record.calibration_transfer_us = static_cast<double>(record.effective_byte_count) * 1'000'000.0 / bandwidth;
        record.runtime_scale = service.runtime_scale;
        projected = (record.calibration_setup_us + record.calibration_transfer_us) * service.runtime_scale;
    }
    const auto duration = projected ? ceil_duration(*projected) : std::nullopt;
    if (!duration) {
        record.status = decision.effect_type == HiCacheEffectType::CommitHostToStorage ? HiCacheIoCostStatus::InconsistentStorageResidency
                                                                                       : HiCacheIoCostStatus::DurationOverflow;
        record.reason = "invalid_service_projection";
        return record;
    }
    record.duration_us = *duration;
    record.resource_lane = resource_lane(decision, model_fields);

    if (control != model_fields.control_models.end() && has_host_control(control->second)) {
        record.host_control_fixed_us = control->second.fixed_us_per_operation * static_cast<double>(record.host_control_operation_count);
        record.host_control_page_us = control->second.per_page_us * static_cast<double>(record.host_control_page_count);
        const auto control_duration = ceil_duration(record.host_control_fixed_us + record.host_control_page_us);
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
