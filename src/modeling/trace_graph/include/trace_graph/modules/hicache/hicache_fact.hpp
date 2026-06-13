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
 * token 可能来自 JSON 数字、字符串或数组。这里统一保留 word 序列，target page
 * hash 会按 word 的确定性字节序重新计算，因此 parser 不依赖 source 侧 page id。
 */
struct HiCacheToken {
    std::vector<uint32_t> words;
};

using HiCacheTokenPath = std::vector<HiCacheToken>;

/**
 * @brief token dictionary 中一段 request path 的引用。
 *
 * span 只描述 token path 的不可变区间；是否能解析成具体 token 取决于 parser 是否
 * 已经观察到对应 dictionary。解析失败时模型会记录缺失 invariant，而不是退回到
 * source_actual state。
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
 * @brief HiCache trace event 被解析后的 atomic fact。
 *
 * 这个结构只承载 trace 中声明的事实字段，不在 parser 层做 target-state 推断。
 * router 会进一步筛选 `model_input=true`、`fact_class=invariant_state` 且
 * `fact_granularity=atomic` 的事实；source_actual、timing_observation、oracle 和
 * debug 信息即使被解析出来，也不能绕过 router 进入状态模型。
 */
struct HiCacheFact {
    size_t source_node_id = 0;
    size_t source_event_index = 0;
    uint64_t ts = 0;
    uint64_t dur = 0;
    std::string event_name;
    std::string target_id;
    std::string fact_class;
    std::string fact_granularity;
    std::string role;
    std::string phase;
    std::string request_id;
    std::string operation_id;
    std::string cache_scope;
    std::string check_kind;
    std::string lifecycle_kind;
    std::string admission_kind;
    uint64_t seq_no = 0;
    uint64_t source_page_size = 0;
    uint64_t token_count = 0;
    uint64_t max_new_tokens = 0;
    uint64_t truncation_align_size = 0;
    int64_t priority = 0;
    bool has_chunked_req = false;
    bool ignore_eos = false;
    bool model_input = false;
    bool dag_input = false;
    bool is_start = false;
    bool is_end = false;

    HiCacheTokenSpan full_path_span;

    HiCacheTokenPath full_path_tokens;
};

/**
 * @brief 将 TraceEvent 解析成 HiCacheFact，并维护 token dictionary。
 *
 * parser 分两轮使用：先观察 dictionary，再解析事实。这样 span-only event 可以在
 * 不复制完整 token path 的情况下恢复 target page projection 所需的 token 序列。
 * parser 本身不决定 fact 是否进入模型。
 */
class HiCacheFactParser {
  public:
    /** @brief 判断一个 trace event 是否属于 HiCache 事件域。 */
    bool is_hicache_event(const TraceEvent & event) const;

    /**
     * @brief 从 HiCache event 中提取 token dictionary。
     *
     * dictionary 是 target page projection 的输入资料，不是状态迁移事实。
     */
    void observe_token_dictionaries(const TraceEvent & event);

    /** @brief 解析单个 HiCache event，保留其 source node 与 event 顺序信息。 */
    HiCacheFact parse(size_t node_id, const TraceEvent & event) const;

  private:
    std::unordered_map<std::string, HiCacheTokenPath> token_paths_;

    /** @brief 解析 JSON span 描述，但不在这里合成 page state。 */
    HiCacheTokenSpan parse_span(const TraceEvent & event, const std::string & key) const;

    /** @brief 使用已观察到的 dictionary 将 span 解析为 token path。 */
    HiCacheTokenPath resolve_span(const HiCacheTokenSpan & span) const;

    /** @brief 接纳一个 dictionary JSON 片段，无法解析时保持静默跳过。 */
    void observe_dictionary_value(const std::string & raw);
};

} // namespace TraceGraph
