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
    MissingResourceScope,
    MissingBandwidth,
    InconsistentStorageResidency,
    MissingHostControlModel,
    DurationOverflow,
    UnsupportedDirection
};

/** @brief One effect's explicit byte/bandwidth cost projection. */
struct HiCacheIoCostRecord {
    std::string effect_id;
    model::HiCacheEffectType effect_type = model::HiCacheEffectType::Loadback;
    model::HiCacheTransferDirection direction = model::HiCacheTransferDirection::None;
    model::HiCacheTargetEffectState target_effect_state = model::HiCacheTargetEffectState::Unresolved;
    /** @brief An executed host-control decision with no effective transfer payload. */
    bool zero_payload_control = false;
    /** @brief Number of target async operations represented by this effect. */
    uint64_t operation_count = 0;
    uint64_t effective_page_count = 0;
    uint64_t effective_byte_count = 0;
    uint64_t storage_existing_page_count = 0;
    uint64_t storage_new_page_count = 0;
    /** Predicted per-operation existing-key H2S shape used by canonical storage cost. */
    std::vector<uint64_t> storage_existing_operation_page_counts;
    uint64_t storage_existing_byte_count = 0;
    uint64_t storage_new_byte_count = 0;
    std::string operation_kind;
    uint64_t bandwidth_bytes_per_sec = 0;
    double calibration_setup_us = 0.0;
    double calibration_transfer_us = 0.0;
    double runtime_scale = 0.0;
    double storage_existing_service_us = 0.0;
    double storage_new_service_us = 0.0;
    uint64_t duration_us = 0;
    uint64_t host_control_page_count = 0;
    uint64_t host_control_operation_count = 0;
    double host_control_fixed_us = 0.0;
    double host_control_page_us = 0.0;
    uint64_t host_control_duration_us = 0;
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

#ifdef DEBUG
/**
 * @brief Audit trail for a diagnostic-only replay with target-observed costs.
 *
 * Oracle costs are applied after target effect decisions have fixed the operation
 * structure.  They must never feed state replay, calibration, or a production
 * prediction.  Blocking and terminal polling are recorded as outcomes rather than
 * force-filled inputs so the replay still tests the predicted dependencies.
 */
struct HiCacheOracleCostReplayAudit {
    std::string status = "disabled";
    uint64_t required_cost_count = 0;
    uint64_t supplied_cost_count = 0;
    uint64_t applied_cost_count = 0;
    uint64_t oracle_service_us = 0;
    uint64_t applied_service_us = 0;
    uint64_t oracle_control_us = 0;
    uint64_t applied_primitive_control_us = 0;
    uint64_t outcome_only_control_us = 0;
    uint64_t observed_blocking_us = 0;
    bool effect_identity_exact = false;
    bool operation_shape_exact = false;
    bool target_e2e_consumed = false;
};
#endif

/** @brief Resource planning result that deliberately does not mutate the DAG. */
struct HiCacheIoResourcePlan {
    std::string status = "not_built";
    bool byte_projection_available = false;
    uint64_t kv_bytes_per_page = 0;
    std::vector<HiCacheIoCostRecord> costs;
    std::vector<HiCacheIoLaneDependency> lane_dependencies;
    std::map<std::string, uint64_t> counts_by_status;
    std::map<std::string, uint64_t> blocker_counts;
#ifdef DEBUG
    HiCacheOracleCostReplayAudit oracle_cost_replay;
#endif

    [[nodiscard]] uint64_t ready_count() const;
};

/** @brief Returns a stable artifact name for one cost status. */
[[nodiscard]] std::string hicache_io_cost_status_name(HiCacheIoCostStatus status);

/** @brief Builds costs and lane order without creating graph mutations. */
[[nodiscard]] HiCacheIoResourcePlan build_hicache_io_resource_plan(const model::HiCacheEffectDecisionLedger & effects,
                                                                   const frontend::HiCacheIoCostConfig & model_fields);

#ifdef DEBUG
/**
 * @brief Strictly replaces model costs with a diagnostic oracle after structure replay.
 *
 * The input is rejected unless it covers every target transfer cost exactly once
 * with matching effect identity and operation shape.  The function is intentionally
 * separate from the cost model so target observations cannot influence prediction.
 */
void apply_hicache_oracle_cost_replay(HiCacheIoResourcePlan & plan, const std::string & filename);
#endif

} // namespace markov::trace_graph::modules::hicache::patch
