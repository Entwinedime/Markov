/**
 * @file
 * @brief Release business contract exported from the HiCache state model to the DAG patcher.
 */
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace markov::trace_graph::modules::hicache::model {

/** @brief Performance-effect categories derivable from state replay. */
enum class HiCacheEffectType : std::uint8_t { Prefetch, Loadback, CommitDeviceToHost, CommitHostToStorage, CommitCapacityGate, Prefill };

/** @brief Data-transfer direction; dependency-only effects use `None`. */
enum class HiCacheTransferDirection : std::uint8_t { None, StorageToHost, HostToDevice, DeviceToHost, HostToStorage };

/** @brief Whether the current prediction contract can safely patch this effect. */
enum class HiCacheEffectPatchStatus : std::uint8_t { Patchable, NotPatchable, Deferred };

/** @brief Target-model logical boundary, not an attribution to a source DAG node. */
struct HiCacheEffectBoundary {
    std::string kind;
    uint64_t epoch = 0;
    /** @brief Timestamp in Chrome trace units, which are microseconds in the active input contract. */
    uint64_t timestamp_us = 0;
};

/**
 * @brief Release effect contract independent of Debug history.
 *
 * Logical boundaries come from the target state machine. Source-node attribution and
 * calibrated target latency remain explicit patchability gaps rather than guessed IDs.
 */
struct HiCacheEffectIntent {
    std::string effect_id;
    HiCacheEffectType effect_type = HiCacheEffectType::Prefetch;
    std::string operation_id;
    std::string request_key;
    std::string cache_scope;
    HiCacheTransferDirection direction = HiCacheTransferDirection::None;
    std::vector<std::string> pages;
    uint64_t candidate_page_count = 0;
    uint64_t effective_page_count = 0;
    uint64_t effective_byte_count = 0;
    HiCacheEffectBoundary logical_start_boundary;
    HiCacheEffectBoundary logical_completion_boundary;
    HiCacheEffectBoundary consumer_boundary;
    std::string resource_lane;
    std::string state;
    HiCacheEffectPatchStatus patch_status = HiCacheEffectPatchStatus::NotPatchable;
    std::string not_patchable_reason;
};

/** @brief Effect intents and explicit contract gaps exported by one state replay. */
struct HiCacheEffectIntentCatalog {
    std::string status = "contract_skeleton";
    uint64_t kv_bytes_per_page = 0;
    bool byte_projection_available = false;
    std::string byte_projection_source;
    std::vector<HiCacheEffectIntent> intents;
    std::map<std::string, uint64_t> missing_facts;
    std::map<std::string, uint64_t> not_patchable_reasons;

    [[nodiscard]] uint64_t patchable_count() const;
    [[nodiscard]] uint64_t not_patchable_count() const;
    [[nodiscard]] uint64_t deferred_count() const;
    [[nodiscard]] std::map<std::string, uint64_t> counts_by_effect_type() const;
};

[[nodiscard]] std::string hicache_effect_type_name(HiCacheEffectType type);
[[nodiscard]] std::string hicache_transfer_direction_name(HiCacheTransferDirection direction);
[[nodiscard]] std::string hicache_effect_patch_status_name(HiCacheEffectPatchStatus status);

} // namespace markov::trace_graph::modules::hicache::model
