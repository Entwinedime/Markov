#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace TraceGraph {

struct TraceEvent;

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
    std::string check_kind;
    std::string lifecycle_kind;
    std::string admission_kind;
    std::string storage_source;

    uint64_t seq_no = 0;
    uint64_t source_page_size = 0;
    uint64_t token_count = 0;
    uint64_t max_new_tokens = 0;
    uint64_t truncation_align_size = 0;
    int64_t priority = 0;
    bool has_chunked_req = false;
    bool ignore_eos = false;
    bool is_start = false;
    bool is_end = false;

    HiCacheTokenSpan full_path_span;
    HiCacheTokenPath full_path_tokens;
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
    [[nodiscard]] bool is_hicache_event(const TraceEvent & event) const;

    /** @brief 从 HiCache event 中观察 token dictionary。 */
    void observe_token_dictionaries(const TraceEvent & event);

    /** @brief 将 HiCache event 解析成 fact。 */
    [[nodiscard]] HiCacheFact parse(size_t node_id, const TraceEvent & event) const;

private:
    std::unordered_map<std::string, HiCacheTokenPath> token_paths_;

    [[nodiscard]] HiCacheTokenSpan parse_span(const TraceEvent & event, const std::string & key) const;
    [[nodiscard]] HiCacheTokenPath resolve_span(const HiCacheTokenSpan & span) const;
    void observe_dictionary_value(const std::string & raw);
};

} // namespace TraceGraph
