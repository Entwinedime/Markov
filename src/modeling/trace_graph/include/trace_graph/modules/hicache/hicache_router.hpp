#pragma once

#include "trace_graph/modules/hicache/hicache_fact.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace TraceGraph {

/**
 * @brief HiCache 状态模型目前接纳的 atomic invariant role。
 *
 * 这个枚举是状态机的白名单。新增 role 时必须同时明确 required fields、handler
 * 语义以及是否属于 target-state 输入；不要用 Unknown 承载兼容分支。
 */
enum class HiCacheFactRole {
    Unknown,
    RequestBoundMatchAnchor,
    RequestLifecycleAnchor,
    RequestAdmission,
    PrefetchDecision,
    PrefetchCheckPoint,
};

/**
 * @brief 单个 HiCacheFact 的路由结果。
 *
 * model_fact 表示 fact 通过了输入契约；known_role 表示 role 在白名单中。二者分开
 * 是为了在 summary 中区分“非模型事实”和“模型事实但 role 未实现/未知”。
 */
struct HiCacheFactRoute {
    bool model_fact = false;
    bool known_role = false;
    HiCacheFactRole role = HiCacheFactRole::Unknown;
};

/**
 * @brief 判断 fact 是否满足 HiCache 状态模型的输入契约。
 *
 * 当前正常模型只消费 `model_input=true && fact_class=invariant_state &&
 * fact_granularity=atomic`。其他 fact 只能作为证据或输出统计，不能驱动 target
 * state。
 */
[[nodiscard]] bool is_hicache_state_model_fact(const HiCacheFact & fact);

/** @brief 将 event_role 字符串解析为白名单 role。 */
[[nodiscard]] HiCacheFactRole parse_hicache_fact_role(const std::string & role);

/** @brief 返回 role 的稳定字符串名，用于 summary 与缺失项统计。 */
[[nodiscard]] std::string hicache_fact_role_name(HiCacheFactRole role);

/** @brief 对 fact 执行模型输入契约检查和 role 白名单路由。 */
[[nodiscard]] HiCacheFactRoute route_hicache_fact(const HiCacheFact & fact);

/** @brief 判断 role 是否已经有状态机 handler。 */
[[nodiscard]] bool hicache_fact_role_implemented(HiCacheFactRole role);

/**
 * @brief 返回 role 对应的必需字段缺失列表。
 *
 * 这里检查的是 target projection 和状态机所需的不变量字段；缺失字段会进入
 * summary.missing_invariant_facts，而不是触发 source-state 兜底路径。
 */
[[nodiscard]] std::vector<std::string> hicache_required_fact_errors(const HiCacheFact & fact, HiCacheFactRole role, uint64_t effective_page_size);

} // namespace TraceGraph
