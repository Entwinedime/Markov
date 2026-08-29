/**
 * @file
 * @brief Narrow model configuration passed from Python orchestration to the C++ backend.
 */
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace markov::trace_graph::frontend {

/** @brief One ordered substring rule for the optional duration-scaling transform. */
struct NodeScaleRuleConfig {
    std::string name{};
    double factor = 1.0;
};

/** @brief Configuration for the framework-neutral NodeScale DAG transform. */
struct NodeScaleConfig {
    bool enabled = false;
    std::vector<NodeScaleRuleConfig> rules{};
};

/** @brief One page-byte anchor in a runtime DMA calibration curve. */
struct HiCacheIoPageBandwidthPoint {
    uint64_t page_bytes = 0;
    double bandwidth_bytes_per_sec = 0.0;
};

/** @brief One existing-key page-size and operation-depth throughput anchor. */
struct HiCacheIoExistingKeyBandwidthPoint {
    uint64_t page_bytes = 0;
    uint64_t operation_pages = 0;
    double bandwidth_bytes_per_sec = 0.0;
};

/** @brief Runtime-batch new-write cost at one page size. */
struct HiCacheIoNewOperationPoint {
    uint64_t page_bytes = 0;
    double setup_us_per_operation = 0.0;
    double bandwidth_bytes_per_sec = 0.0;
};

/** @brief One direction-specific physical service curve in the unified I/O model. */
struct HiCacheIoServiceModelConfig {
    std::string direction{};
    double setup_us_per_operation = 0.0;
    double setup_us_per_page = 0.0;
    double bandwidth_bytes_per_sec = 0.0;
    double runtime_scale = 1.0;
    std::vector<HiCacheIoPageBandwidthPoint> page_bandwidth_points{};
    std::vector<HiCacheIoNewOperationPoint> new_operation_points{};
    std::vector<HiCacheIoExistingKeyBandwidthPoint> existing_key_bandwidth_points{};
    double existing_runtime_scale = 1.0;
    double new_runtime_scale = 1.0;
};

/** @brief Explicit per-operation and per-page host control primitive. */
struct HiCacheIoControlModelConfig {
    double fixed_us_per_operation = 0.0;
    double zero_payload_fixed_us_per_operation = 0.0;
    double per_page_us = 0.0;
};

/** @brief Whether storage directions share one server lane or remain scope-local. */
struct HiCacheIoResourceLanesConfig {
    bool shared_storage_read = false;
    bool shared_storage_write = false;
};

/** @brief Numerical fields consumed by the HiCache direct I/O cost model. */
struct HiCacheIoCostConfig {
    std::map<std::string, HiCacheIoServiceModelConfig> service_models{};
    std::map<std::string, HiCacheIoControlModelConfig> control_models{};
    HiCacheIoResourceLanesConfig resource_lanes{};
};

/** @brief Calibration-only rates used to resolve target I/O readiness during effect replay. */
struct HiCacheIoPlanningConfig {
    uint64_t device_host_bandwidth_bytes_per_sec = 0;
    uint64_t host_storage_bandwidth_bytes_per_sec = 0;
};

/**
 * @brief Explicit target configuration consumed by HiCache state replay.
 *
 * These fields describe target policy, capacity, and byte projection. Source-observed policy
 * outcomes are not accepted here; replay derives target behavior from approved facts.
 */
struct HiCacheConfig {
    bool enabled = false;
    uint64_t page_size = 0;
    uint64_t kv_bytes_per_page = 0;
    uint64_t l1_capacity_pages = 0;
    uint64_t l2_capacity_pages = 0;
    std::string write_policy = "write_through";
    uint64_t write_through_threshold = 0;
    std::string prefetch_policy = "timeout";
    uint64_t prefetch_threshold_pages = 0;
    uint64_t prefetch_capacity_limit_pages = 0;
    bool prefetch_timeout_configured = false;
    double prefetch_timeout_base_sec = 0.0;
    double prefetch_timeout_per_ki_token_sec = 0.0;
    double prefetch_timeout_max_sec = 0.0;
    bool device_allocator_need_sort = false;
    HiCacheIoPlanningConfig io_planning;
    HiCacheIoCostConfig io_cost;
    bool dag_patch_enabled = false;
    bool dag_patch_source_target_same_config = false;
};

/**
 * @brief Complete narrow configuration understood by the C++ backend.
 *
 * Python converts the broader workflow configuration into this boundary document. The C++
 * parser intentionally does not understand experiment orchestration fields.
 */
struct ModelConfig {
    NodeScaleConfig node_scale;
    HiCacheConfig hicache;

    /** @brief Loads and validates one narrow model-configuration JSON file. */
    [[nodiscard]] static ModelConfig from_file(const std::string & filename);
};

} // namespace markov::trace_graph::frontend
