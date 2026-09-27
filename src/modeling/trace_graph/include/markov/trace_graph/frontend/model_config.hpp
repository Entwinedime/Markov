/**
 * @file
 * @brief Narrow model configuration passed from Python orchestration to the C++ backend.
 */
#pragma once

#include <cstdint>
#include <map>
#include <optional>
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

/** @brief Setup plus bandwidth at one page size, for DMA or new-key storage. */
struct HiCacheIoTransferPoint {
    uint64_t page_bytes = 0;
    double bandwidth_bytes_per_sec = 0.0;
    double setup_us_per_operation = 0.0;
};

/** @brief One existing-key page-size and operation-depth throughput anchor. */
struct HiCacheIoExistingKeyBandwidthPoint {
    uint64_t page_bytes = 0;
    uint64_t operation_pages = 0;
    double bandwidth_bytes_per_sec = 0.0;
};

/** @brief File read/copy/publication stages measured by fixed physical calibration. */
struct HiCachePrefetchStagesConfig {
    double before_copy_us_per_page = 0.0;
    double before_copy_us_per_byte = 0.0;
    double copy_publish_us_per_page = 0.0;
    double copy_publish_us_per_byte = 0.0;
    double return_us_per_operation = 0.0;
    double return_us_per_page = 0.0;
};

/** @brief One direction-specific physical service curve in the unified I/O model. */
struct HiCacheIoServiceModelConfig {
    std::string direction{};
    double runtime_scale = 1.0;
    double existing_runtime_scale = 1.0;
    std::vector<HiCacheIoTransferPoint> page_bandwidth_points{};
    std::vector<HiCacheIoTransferPoint> new_operation_points{};
    std::vector<HiCacheIoExistingKeyBandwidthPoint> existing_key_bandwidth_points{};
    std::optional<HiCachePrefetchStagesConfig> stages;
};

/** @brief One measured fixed host-control cost per I/O operation. */
struct HiCacheIoControlModelConfig {
    double fixed_us_per_operation = 0.0;
};

/** @brief Whether storage directions share one server lane or remain scope-local. */
struct HiCacheIoResourceLanesConfig {
    bool shared_storage_read = false;
    bool shared_storage_write = false;
};

/** @brief Numerical fields consumed by the HiCache direct I/O cost model. */
struct HiCacheIoCostConfig {
    uint64_t storage_batch_pages = 0;
    std::map<std::string, HiCacheIoServiceModelConfig> service_models{};
    std::map<std::string, HiCacheIoControlModelConfig> control_models{};
    HiCacheIoResourceLanesConfig resource_lanes{};
    /** Independent CPU-envelope calibration, per empty blocking write check. */
    std::optional<double> empty_write_check_us;
    std::optional<double> locked_candidate_us;
    double locked_candidate_log2_heap_us = 0.0;
};

/** @brief Monotone cost response for one source-owned phase node family. */
struct HiCachePhaseLinearCostConfig {
    double fixed_us = 0.0;
    double per_new_token_us = 0.0;
    double per_attention_token_pair_us = 0.0;
    double per_context_token_us = 0.0;
};

/** @brief One measured point in a phase cost curve indexed by newly computed tokens. */
struct HiCachePhaseTokenCostPoint {
    uint64_t new_tokens = 0;
    double duration_us = 0.0;
};

/** @brief Piecewise-linear phase cost derived directly from measured token-work anchors. */
struct HiCachePhaseTokenCostConfig {
    std::vector<HiCachePhaseTokenCostPoint> points{};
};

/** @brief Machine-level paged-attention work/cost primitive. */
struct HiCacheDecodePagedAttentionCostConfig {
    uint64_t kernel_page_tokens = 0;
    double fixed_us_per_iteration = 0.0;
    double per_context_token_us = 0.0;
    double per_effective_page_us = 0.0;
};

/** @brief Shared machine primitives plus the selected-base-only templates. */
struct HiCachePhaseCostConfig {
    bool enabled = false;
    HiCachePhaseTokenCostConfig prefill_common_kernel;
    HiCachePhaseTokenCostConfig prefill_collective;
    HiCachePhaseLinearCostConfig prefill_prefix_attention;
    std::optional<HiCacheDecodePagedAttentionCostConfig> decode_paged_attention;
    std::optional<HiCachePhaseLinearCostConfig> decode_collective;
    uint64_t min_new_tokens = 0;
    uint64_t max_new_tokens = 0;
    uint64_t min_context_tokens = 0;
    uint64_t max_context_tokens = 0;
    double min_attention_token_pairs = 0.0;
    double max_attention_token_pairs = 0.0;
    uint64_t min_decode_context_tokens = 0;
    uint64_t max_decode_context_tokens = 0;
    uint64_t base_page_size = 0;
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
    std::string prefetch_wait_calibration;              // Independent branch timing, used only when base has no active wait.
    std::string prefetch_query_calibration;             // Independent query CPU/communication costs, not a source DAG.
    std::string load_index_calibration;                 // Independent index-operation costs; no target topology.
    std::string load_submission_calibration;            // Independent Ascend submission costs, not a target DAG.
    std::string layer_wait_calibration;                 // Independent event-wait submission costs.
    std::vector<std::string> prefetch_cpu_calibrations; // Policy-independent local return costs only.
    std::string write_host_calibration;                 // Independent host-call templates for write policies absent from the base.
    std::string release_host_calibration;               // Independent ordinary release work, rebound to base allocator resources.
    std::string write_confirmation_calibration;         // Independent nonblocking ACK tails, including empty checks.
    uint64_t prefetch_threshold_pages = 0;
    uint64_t prefetch_capacity_limit_pages = 0;
    bool prefetch_timeout_configured = false;
    double prefetch_timeout_base_sec = 0.0;
    double prefetch_timeout_per_ki_token_sec = 0.0;
    double prefetch_timeout_max_sec = 0.0;
    bool device_allocator_need_sort = false;
    HiCacheIoCostConfig io_cost;
    HiCachePhaseCostConfig phase_cost;
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
    std::string source_prefetch_policy; // Source observation context, not a target policy/outcome.

    /** @brief Loads and validates one narrow model-configuration JSON file. */
    [[nodiscard]] static ModelConfig from_file(const std::string & filename);
};

} // namespace markov::trace_graph::frontend
