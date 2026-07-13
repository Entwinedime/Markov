/**
 * @file
 * @brief Parsed HiCache probe facts and token-dictionary hydration.
 */
#pragma once

#include "markov/trace_graph/core/trace_event.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace markov::trace_graph::modules::hicache {

/**
 * @brief Canonical representation of one HiCache token.
 *
 * Probes may encode a token as a number, numeric string, or word array. Replay retains only
 * the unsigned 32-bit words that participate in target-page hashing.
 */
struct HiCacheToken {
    std::vector<uint32_t> words{};

    bool operator==(const HiCacheToken &) const = default;
};

using HiCacheTokenPath = std::vector<HiCacheToken>;

/** @brief Catalog metadata shared by state routing and source-DAG attribution. */
struct HiCacheFactMetadata {
    std::string fact_class;
    std::string role;
    std::vector<std::string> consumers;
};

/**
 * @brief Parses the required `args.fact` object without hydrating token paths.
 *
 * This is the narrow parser for code that needs catalog identity but must not
 * construct a second state-model fact table.
 */
[[nodiscard]] HiCacheFactMetadata parse_hicache_fact_metadata(const core::TraceEvent & event);

/** @brief Returns the semantic boundary timestamp of a catalog phase event. */
[[nodiscard]] uint64_t hicache_fact_boundary_timestamp(const core::TraceEvent & event);

/**
 * @brief Immutable reference to a half-open interval in a token dictionary path.
 *
 * A span carries no source page identity, so the same token interval can be projected under
 * a different target page size without consuming a source residency result.
 */
struct HiCacheTokenSpan {
    std::string path_id{};
    uint64_t begin = 0;
    uint64_t end = 0;
    uint64_t token_count = 0;
    std::string hash_algo{};
    bool valid = false;
};

/** @brief Parses one token-span argument without resolving its token dictionary. */
[[nodiscard]] HiCacheTokenSpan parse_hicache_token_span_arg(const core::TraceEvent & event, std::string_view key);

/**
 * @brief Accepted fill path for one request in a `cache_extend_input` batch.
 *
 * Batch entries stay independent from the scalar path so the first request cannot
 * accidentally become a per-request fallback for the rest of the batch.
 */
struct HiCacheBatchPathEntry {
    std::string request_id;
    uint64_t position = 0;
    HiCacheTokenSpan full_path_span;
    HiCacheTokenPath full_path_tokens;
    uint64_t token_count = 0;
};

/**
 * @brief Atomic fact parsed from one HiCache trace event.
 *
 * This record carries only catalog-declared fields. The router separately enforces class,
 * role, phase, and consumer eligibility before target replay.
 */
struct HiCacheFact {
    size_t source_node_id = 0;
    size_t source_event_index = 0;
    uint64_t ts = 0;  ///< Semantic phase-boundary timestamp in microseconds.
    uint64_t dur = 0; ///< Chrome trace duration in microseconds.

    std::string event_name;
    std::string target_id;
    std::string fact_class;
    std::string role;
    std::vector<std::string> consumers;
    std::string phase;

    std::string request_id;
    std::string operation_id;
    std::string cache_scope;
    std::string lifecycle_kind;
    std::string batch_kind;

    uint64_t seq_no = 0;
    uint64_t source_page_size = 0;
    uint64_t token_count = 0;
    uint64_t batch_size = 0;
    uint64_t batch_request_id_count = 0;
    uint64_t batch_position_count = 0;
    uint64_t batch_token_dictionary_count = 0;
    uint64_t batch_span_count = 0;
    uint64_t batch_token_count_count = 0;
    int64_t priority = 0;
    bool is_start = false;
    bool is_end = false;
    bool batch_request_ids_array = false;
    bool batch_positions_array = false;
    bool batch_token_dictionaries_array = false;
    bool batch_spans_array = false;
    bool batch_token_counts_array = false;
    bool batch_request_ids_unique = false;
    bool batch_positions_cover_indexes = false;
    bool batch_positions_match_request_ids = true;

    HiCacheTokenSpan full_path_span;
    HiCacheTokenPath full_path_tokens;
    std::vector<HiCacheBatchPathEntry> batch_paths;
    /** @brief Returns whether catalog metadata declares the requested consumer. */
    [[nodiscard]] bool has_consumer(std::string_view consumer) const;
};

/**
 * @brief Returns whether the fact-local full path is complete enough for replay.
 *
 * An explicit zero-length span is valid and means no complete target page. A non-empty span
 * must hydrate to exactly its declared token count; no other role or source result fills gaps.
 */
[[nodiscard]] bool hicache_fact_has_resolved_full_path(const HiCacheFact & fact);

/**
 * @brief HiCache event parser and approved token-dictionary index.
 *
 * Callers first observe dictionaries from approved state-model path facts, then parse
 * span-only facts. Dictionaries carried by source-actual, oracle, or diagnostics facts are
 * intentionally excluded from normal replay hydration.
 */
class HiCacheFactParser {
public:
    /** @brief Returns whether an event belongs to the HiCache trace domain. */
    [[nodiscard]] bool is_hicache_event(const core::TraceEvent & event) const;

    /** @brief Indexes token dictionaries only when the event passes the source contract. */
    void observe_token_dictionaries(const core::TraceEvent & event);

    /** @brief Parses and normalizes one HiCache event without routing it. */
    [[nodiscard]] HiCacheFact parse(size_t node_id, const core::TraceEvent & event) const;

private:
    std::unordered_map<std::string, HiCacheTokenPath> token_paths_;

    [[nodiscard]] HiCacheTokenSpan parse_span(const core::TraceEvent & event, std::string_view key) const;
    [[nodiscard]] HiCacheTokenPath resolve_span(const HiCacheTokenSpan & span) const;
    void parse_batch_fields(HiCacheFact & fact, const core::TraceEvent & event) const;
    void observe_dictionary_value(const std::string & raw);
};

} // namespace markov::trace_graph::modules::hicache
