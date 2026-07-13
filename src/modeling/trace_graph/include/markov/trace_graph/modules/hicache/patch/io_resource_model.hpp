/**
 * @file
 * @brief Read-only HiCache I/O costs and deterministic resource-lane ordering.
 */
#pragma once

#include "markov/trace_graph/modules/hicache/model/effect_decision.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace markov::trace_graph::modules::hicache::patch {

/** @brief Cost readiness for one target-derived effect decision. */
enum class HiCacheIoCostStatus : std::uint8_t {
    Ready,
    NotRequired,
    Deferred,
    Unresolved,
    MissingEffectiveBytes,
    MissingByteProjection,
    ByteProjectionOverflow,
    MissingIoModelIdentity,
    MissingResourceModel,
    UnsupportedResourceModel,
    MissingIoModelProvenance,
    MissingResourceScope,
    MissingBandwidth,
    DurationOverflow,
    UnsupportedDirection
};

/** @brief One effect's explicit byte/bandwidth cost projection. */
struct HiCacheIoCostRecord {
    std::string effect_id;
    model::HiCacheEffectType effect_type = model::HiCacheEffectType::Loadback;
    model::HiCacheTransferDirection direction = model::HiCacheTransferDirection::None;
    model::HiCacheTargetEffectState target_effect_state = model::HiCacheTargetEffectState::Unresolved;
    uint64_t effective_byte_count = 0;
    std::string bandwidth_field;
    uint64_t bandwidth_bytes_per_sec = 0;
    uint64_t duration_us = 0;
    std::string resource_scope;
    std::string resource_lane;
    uint64_t logical_order_epoch = 0;
    HiCacheIoCostStatus status = HiCacheIoCostStatus::Unresolved;
    std::string reason;
};

/** @brief Ordering between adjacent cost-ready operations on one logical lane. */
struct HiCacheIoLaneDependency {
    std::string resource_lane;
    std::string predecessor_effect_id;
    std::string successor_effect_id;
};

/** @brief Resource planning result that deliberately does not mutate the DAG. */
struct HiCacheIoResourcePlan {
    std::string status = "not_built";
    std::string io_model_id;
    std::string io_model_digest;
    std::string io_model_calibration_status;
    std::string resource_model_id;
    std::string byte_projection_source;
    uint64_t kv_bytes_per_page = 0;
    uint64_t device_host_bandwidth_bytes_per_sec = 0;
    uint64_t host_storage_bandwidth_bytes_per_sec = 0;
    std::map<std::string, std::string> io_model_provenance;
    std::vector<HiCacheIoCostRecord> costs;
    std::vector<HiCacheIoLaneDependency> lane_dependencies;
    std::map<std::string, uint64_t> counts_by_status;
    std::map<std::string, uint64_t> blocker_counts;

    [[nodiscard]] uint64_t ready_count() const;
    /** @brief Returns whether this model may authorize a production graph mutation. */
    [[nodiscard]] bool calibrated_for_apply() const;
};

/** @brief Returns a stable artifact name for one cost status. */
[[nodiscard]] std::string hicache_io_cost_status_name(HiCacheIoCostStatus status);

/** @brief Builds costs and lane order without creating graph mutations. */
[[nodiscard]] HiCacheIoResourcePlan build_hicache_io_resource_plan(const model::HiCacheEffectDecisionLedger & decisions);

} // namespace markov::trace_graph::modules::hicache::patch
