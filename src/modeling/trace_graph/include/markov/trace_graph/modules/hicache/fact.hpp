/**
 * @file
 * @brief HiCache probe fact 的解析结果和 token dictionary 水合器。
 */
#pragma once

#include "markov/trace_graph/core/trace_event.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace markov::trace_graph::modules::hicache {

/**
 * @brief HiCache token 的规范化表示。
 *
 * probe 可能把 token 写成数字、字符串数字或复合数组。模型层不保存 Python
 * 对象形态，只保留参与 target page hash 的 32-bit word 序列。
 */
struct HiCacheToken {
    std::vector<uint32_t> words;
};

using HiCacheTokenPath = std::vector<HiCacheToken>;

/**
 * @brief token dictionary 中一段 request path 的不可变引用。
 *
 * span 只说明“哪条 token path 的哪个半开区间”，不携带 source page identity，
 * 因而可以安全用于 cross-config target page projection。
 */
struct HiCacheTokenSpan {
    std::string path_id;
    uint64_t begin = 0;
    uint64_t end = 0;
    uint64_t token_count = 0;
    std::string hash_algo;
    bool valid = false;
};

/**
 * @brief `cache_extend_input` batch 中单个 request 的 accepted fill path。
 *
 * batch entry 独立于 scalar request path，避免把 batch 第一项误写进
 * `HiCacheFact::full_path_tokens` 并退化回 per-request 输入语义。
 */
struct HiCacheBatchPathEntry {
    std::string request_id;
    uint64_t position = 0;
    HiCacheTokenSpan full_path_span;
    HiCacheTokenPath full_path_tokens;
    uint64_t token_count = 0;
};

/**
 * @brief 单条 HiCache trace event 解析后的 atomic fact。
 *
 * 该结构只承载 probe catalog 显式声明的字段。是否能进入 target state model，
 * 由 router 基于 `fact.class`、`fact.role` 和 `fact.consumers` 再做硬门禁。
 */
struct HiCacheFact {
    size_t source_node_id = 0;
    size_t source_event_index = 0;
    uint64_t ts = 0;
    uint64_t dur = 0;

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
    std::string storage_source;
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
    std::vector<std::string> storage_page_hashes;

    /** @brief 判断 catalog fact 是否声明给指定 consumer 消费。 */
    [[nodiscard]] bool has_consumer(const std::string & consumer) const;
};

/**
 * @brief 判断 fact-local full path 是否已经可被模型消费。
 *
 * 空 path 是合法输入：当 span 明确指向长度为 0 的 token 区间时，它表示本轮没有
 * 可投影 page，而不是 token dictionary 缺失。非空 path 则必须已经通过
 * token dictionary 水合出完整 token 序列。
 */
[[nodiscard]] bool hicache_fact_has_resolved_full_path(const HiCacheFact & fact);

/**
 * @brief HiCache event parser 和 token dictionary 索引。
 *
 * parser 先从 completed state-model path fact 中观察 dictionary，再解析 span-only
 * fact。source actual / oracle 中的 dictionary 只可用于诊断，不能水合 normal model
 * 的 token path。
 */
class HiCacheFactParser {
public:
    /** @brief 判断一个 TraceEvent 是否属于 HiCache 域。 */
    [[nodiscard]] bool is_hicache_event(const core::TraceEvent & event) const;

    /** @brief 从 HiCache event 中观察 token dictionary。 */
    void observe_token_dictionaries(const core::TraceEvent & event);

    /** @brief 将 HiCache event 解析成 fact。 */
    [[nodiscard]] HiCacheFact parse(size_t node_id, const core::TraceEvent & event) const;

private:
    std::unordered_map<std::string, HiCacheTokenPath> token_paths_;

    [[nodiscard]] HiCacheTokenSpan parse_span(const core::TraceEvent & event, const std::string & key) const;
    [[nodiscard]] HiCacheTokenPath resolve_span(const HiCacheTokenSpan & span) const;
    [[nodiscard]] std::vector<HiCacheBatchPathEntry> parse_batch_paths(const core::TraceEvent & event) const;
    void observe_dictionary_value(const std::string & raw);
};

} // namespace markov::trace_graph::modules::hicache
