#pragma once

#include "trace_graph/modules/hicache/hicache_fact.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace TraceGraph {

/**
 * @brief request 维度的 token path 暂存器。
 *
 * 部分 atomic invariant fact 只携带 span 或生命周期字段，需要复用更早 request-bound
 * fact 中的 token context。store 只保存 token，不保存 page residency，避免把 lookup
 * 观测结果误当作 target state。
 */
class HiCacheTokenPathStore {
  public:
    /** @brief 返回 scope/request 复合 key；缺少 request_id 时返回空字符串。 */
    std::string scoped_request_key(const HiCacheFact & fact) const;

    /** @brief 保存 request token path，只允许更完整的 path 覆盖较短 path。 */
    void set_request_tokens(const HiCacheFact & fact, const HiCacheTokenPath & tokens);

    /** @brief 记录 request-bound anchor，作为后续 projection 的可追溯来源。 */
    void observe_request_bound_tokens(const HiCacheFact & fact, const HiCacheTokenPath & tokens);

    /** @brief 查询 request 当前已知的 token path。 */
    HiCacheTokenPath request_tokens(const HiCacheFact & fact) const;

  private:
    /**
     * @brief request-bound token anchor。
     *
     * 当前状态机主要按 request key 查询 token；anchor 列表保留去重后的观察点，方便
     * 后续需要审计 request-bound projection 来源时扩展。
     */
    struct RequestBoundAnchor {
        std::string scope;
        HiCacheTokenPath tokens;
    };

    std::unordered_map<std::string, HiCacheTokenPath> request_tokens_by_key_;
    std::vector<RequestBoundAnchor> request_bound_anchors_;
};

} // namespace TraceGraph
