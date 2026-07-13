/**
 * @file
 * @brief Narrow model configuration passed from Python orchestration to the C++ backend.
 */
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace markov::trace_graph::frontend {

/**
 * @brief One duration-scaling rule for `NodeScaleModule`.
 *
 * `name` is a literal substring, not a regular expression. Rules are ordered and the first
 * match scales the complete node duration by the positive finite `factor`.
 */
struct NodeScaleRuleConfig {
    std::string id{};
    std::string name{};
    double factor = 1.0;
};

/** @brief Narrow NodeScale configuration containing activation and ordered rules only. */
struct NodeScaleConfig {
    bool enabled = false;
    std::vector<NodeScaleRuleConfig> rules{};
};

/** @brief Explicit model identity and the only two HiCache I/O cost parameters. */
struct HiCacheIoCostConfig {
    std::string model_id{};
    std::string model_digest{};
    std::string calibration_status{};
    std::string resource_model{};
    uint64_t device_host_bandwidth_bytes_per_sec = 0;
    uint64_t host_storage_bandwidth_bytes_per_sec = 0;
    std::map<std::string, std::string> provenance{};
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
    HiCacheIoCostConfig io_cost;
#ifdef DEBUG
    bool emit_state_digests = false;
#endif
    bool dag_patch_enabled = false;
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
