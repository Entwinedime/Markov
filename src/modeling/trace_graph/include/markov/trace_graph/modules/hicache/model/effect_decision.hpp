/**
 * @file
 * @brief Complete target-derived HiCache effect decision ledger.
 */
#pragma once

#include "markov/trace_graph/frontend/model_config.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace markov::trace_graph::modules::hicache::model {

/** @brief Direct HiCache effects supported by the first DAG-patch contract. */
enum class HiCacheEffectType : std::uint8_t {
    Loadback,
    PrefetchIo,
    PrefetchVisibility,
    CommitDeviceToHost,
    CommitHostToStorage,
    CommitCapacityGate,
};

/** @brief Data-transfer direction; dependency-only effects use `None`. */
enum class HiCacheTransferDirection : std::uint8_t { None, StorageToHost, HostToDevice, DeviceToHost, HostToStorage };

/** @brief Whether the current prediction contract can safely patch this effect. */
enum class HiCacheEffectPatchStatus : std::uint8_t { Patchable, NotPatchable, Deferred };

/** @brief Explicit target decision for one stable effect opportunity. */
enum class HiCacheTargetEffectState : std::uint8_t { Required, NotRequired, Partial, Deferred, Unresolved };

/** @brief Whether request-arrival spacing can change an effect's direct shape. */
enum class HiCacheScheduleSensitivity : std::uint8_t { ScheduleInvariant, ArrivalScheduleSensitive };

/** @brief Source-DAG carrier classification populated by source attribution. */
enum class HiCacheSourceCarrierState : std::uint8_t { NotEvaluated, Present, Absent, Unobservable, Ambiguous };

/** @brief Target-model logical boundary, not an attribution to a source DAG node. */
struct HiCacheEffectBoundary {
    std::string kind;
    uint64_t epoch = 0;
    /** @brief Timestamp in Chrome trace units, which are microseconds in the active contract. */
    uint64_t timestamp_us = 0;
    /** @brief Semantic fact identity, retained even when no executable anchor exists. */
    std::optional<size_t> source_node_id = std::nullopt;
    /** @brief Optional executable DAG anchor resolved from the semantic fact. */
    std::optional<size_t> execution_anchor_node_id = std::nullopt;
    std::optional<size_t> source_event_index = std::nullopt;
    std::string source_fact_role;
};

/**
 * @brief One target-page segment under a config-independent token-range identity.
 *
 * The parent opportunity remains stable when page size changes. Segment identities use
 * the canonical token-path identity and absolute token interval, while `target_page_id`
 * records the target-specific projection needed by state replay and mutation planning.
 */
struct HiCacheEffectSegment {
    std::string segment_key;
    std::string token_path_id;
    uint64_t token_begin = 0;
    uint64_t token_end = 0;
    std::string target_page_id;
};

/**
 * @brief A config-independent decision point registered before target behavior runs.
 *
 * Every supported lookup, prefetch-candidate, or lifecycle fact creates a fixed set of
 * opportunities. Target policy may later materialize zero or more operations, but it
 * cannot remove the opportunity itself from the ledger.
 */
struct HiCacheEffectOpportunity {
    std::string opportunity_key;
    std::string effect_family_key;
    HiCacheEffectType effect_type = HiCacheEffectType::Loadback;
    HiCacheTransferDirection direction = HiCacheTransferDirection::None;
    std::string cache_scope;
    std::string state_scope_key;
    std::string request_identity;
    std::string request_id_provenance;
    std::string source_fact_role;
    uint64_t source_fact_ordinal = 0;
    uint64_t decision_ordinal = 0;
    uint64_t source_fact_seq_no = 0;
    /** @brief Semantic fact identity, not necessarily an executable DAG node. */
    size_t source_node_id = 0;
    /** @brief Optional proven executable anchor for the opportunity fact. */
    std::optional<size_t> source_execution_anchor_node_id = std::nullopt;
    size_t source_event_index = 0;
    bool input_ready = false;
    HiCacheEffectBoundary eligibility_boundary;
    std::string resource_lane;
    std::vector<HiCacheEffectSegment> candidate_segments;
};

/**
 * @brief One explicit target decision consumed by source attribution and DAG rewriting.
 *
 * `operation_ids` are target-local provenance only. Cross-config joins use
 * `opportunity_key` and child `segment_key` values, never operation IDs or timestamps.
 */
struct HiCacheEffectDecision {
    std::string effect_key;
    std::string opportunity_key;
    std::string effect_family_key;
    HiCacheEffectType effect_type = HiCacheEffectType::Loadback;
    HiCacheTransferDirection direction = HiCacheTransferDirection::None;
    std::string cache_scope;
    std::string request_id_provenance;
    std::string source_fact_role;
    uint64_t source_fact_ordinal = 0;
    /** @brief Semantic fact identity, not necessarily an executable DAG node. */
    size_t source_node_id = 0;
    /** @brief Optional proven executable anchor for the source opportunity. */
    std::optional<size_t> source_execution_anchor_node_id = std::nullopt;
    std::vector<std::string> operation_ids;
    /**
     * Per-operation H2S shape retained from predicted target state replay.
     * Vectors are emitted only when they conserve the corresponding aggregate
     * page count and their indices align with `operation_ids`.
     */
    std::vector<uint64_t> storage_existing_operation_page_counts;
    std::vector<HiCacheEffectSegment> candidate_segments;
    std::vector<HiCacheEffectSegment> effective_segments;
    std::vector<std::string> effective_pages;
    uint64_t effective_page_count = 0;
    uint64_t effective_byte_count = 0;
    /** @brief H2S pages already readable in the predicted target storage directory. */
    uint64_t storage_existing_page_count = 0;
    /** @brief H2S pages requiring a new backend file write in the predicted target state. */
    uint64_t storage_new_page_count = 0;
    HiCacheEffectBoundary eligibility_boundary;
    HiCacheEffectBoundary consumer_boundary;
    std::string resource_lane;
    std::string state;
    HiCacheTargetEffectState target_effect_state = HiCacheTargetEffectState::Unresolved;
    HiCacheScheduleSensitivity schedule_sensitivity = HiCacheScheduleSensitivity::ScheduleInvariant;
    HiCacheSourceCarrierState source_carrier_state = HiCacheSourceCarrierState::NotEvaluated;
    HiCacheEffectPatchStatus patch_status = HiCacheEffectPatchStatus::NotPatchable;
    std::string reason;
    std::string not_patchable_reason;
};

/** @brief Complete target-derived effect plan, independent of cost coefficients. */
struct HiCacheEffectDecisionLedger {
    std::string status = "ready";
    std::string decision_coverage = "complete_target_opportunities";
    std::string prefill_effect_status = "deferred";
    uint64_t kv_bytes_per_page = 0;
    uint64_t l2_capacity_pages = 0;
    uint64_t l2_capacity_bytes = 0;
    bool byte_projection_available = false;
    std::string byte_projection_source;
    std::vector<HiCacheEffectDecision> decisions;
    std::map<std::string, uint64_t> missing_facts;
    std::map<std::string, uint64_t> not_patchable_reasons;

    [[nodiscard]] uint64_t patchable_count() const;
    [[nodiscard]] uint64_t not_patchable_count() const;
    [[nodiscard]] uint64_t deferred_count() const;
    [[nodiscard]] uint64_t unresolved_count() const;
    [[nodiscard]] uint64_t schedule_sensitive_count() const;
    [[nodiscard]] std::map<std::string, uint64_t> counts_by_effect_type() const;
    [[nodiscard]] std::map<std::string, uint64_t> counts_by_target_effect_state() const;
    [[nodiscard]] std::map<std::string, uint64_t> counts_by_schedule_sensitivity() const;
    [[nodiscard]] std::map<std::string, uint64_t> counts_by_source_carrier_state() const;
};

[[nodiscard]] std::string hicache_effect_type_name(HiCacheEffectType type);
[[nodiscard]] std::string hicache_transfer_direction_name(HiCacheTransferDirection direction);
[[nodiscard]] std::string hicache_effect_patch_status_name(HiCacheEffectPatchStatus status);
[[nodiscard]] std::string hicache_target_effect_state_name(HiCacheTargetEffectState state);
[[nodiscard]] std::string hicache_schedule_sensitivity_name(HiCacheScheduleSensitivity sensitivity);
[[nodiscard]] std::string hicache_source_carrier_state_name(HiCacheSourceCarrierState state);

} // namespace markov::trace_graph::modules::hicache::model
