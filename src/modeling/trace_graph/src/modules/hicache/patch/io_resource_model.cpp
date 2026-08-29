/**
 * @file
 * @brief Read-only HiCache I/O resource-model implementation.
 */
#include "markov/trace_graph/modules/hicache/patch/io_resource_model.hpp"

#include "io_resource_cost.hpp"

#include "markov/trace_graph/core/numeric.hpp"

#include <utility>

namespace markov::trace_graph::modules::hicache::patch {

uint64_t HiCacheIoResourcePlan::ready_count() const {
    const auto it = counts_by_status.find("ready");
    return it == counts_by_status.end() ? 0 : it->second;
}

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
    case HiCacheIoCostStatus::MissingResourceScope:
        return "missing_resource_scope";
    case HiCacheIoCostStatus::MissingBandwidth:
        return "missing_bandwidth";
    case HiCacheIoCostStatus::InconsistentStorageResidency:
        return "inconsistent_storage_residency";
    case HiCacheIoCostStatus::MissingHostControlModel:
        return "missing_host_control_model";
    case HiCacheIoCostStatus::DurationOverflow:
        return "duration_overflow";
    case HiCacheIoCostStatus::UnsupportedDirection:
        return "unsupported_direction";
    }
    return "unknown";
}

HiCacheIoResourcePlan build_hicache_io_resource_plan(const model::HiCacheEffectDecisionLedger & decisions,
                                                      const frontend::HiCacheIoCostConfig & model_fields) {
    HiCacheIoResourcePlan plan;
    plan.byte_projection_available = decisions.byte_projection_available;
    plan.kv_bytes_per_page = decisions.kv_bytes_per_page;
    plan.costs.reserve(decisions.decisions.size());
    for (const auto & decision : decisions.decisions) {
        auto cost = io_resource_model_detail::cost_record(decision, decisions, model_fields);
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
