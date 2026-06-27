#pragma once

#include "trace_graph/modules/hicache/hicache_fact.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace TraceGraph {

/**
 * @brief C++ HiCache state model 当前接受的 fact role。
 *
 * Unknown 只用于 summary 诊断，不能承载兼容性分支。
 */
enum class HiCacheFactRole {
    Unknown,
    RequestBoundMatchAnchor,
    RequestLifecycleAnchor,
    RequestAdmission,
    PrefetchDecision,
    PrefetchCheckPoint,
    StorageBackendReadable,
};

/**
 * @brief fact 路由结果。
 *
 * `model_fact` 表示 fact 声明给 `hicache_state_model` 消费；`known_role` 表示
 * class/role 在当前 state model 白名单中。二者分开可区分非模型事实和模型事实 schema 漂移。
 */
struct HiCacheFactRoute {
    bool model_fact = false;
    bool known_role = false;
    HiCacheFactRole role = HiCacheFactRole::Unknown;
};

/** @brief 将 fact.role 字符串解析成白名单枚举。 */
[[nodiscard]] HiCacheFactRole parse_hicache_fact_role(const std::string & role);

/** @brief 返回 role 的稳定字符串名。 */
[[nodiscard]] std::string hicache_fact_role_name(HiCacheFactRole role);

/** @brief 对 fact 执行输入契约检查和 role 路由。 */
[[nodiscard]] HiCacheFactRoute route_hicache_fact(const HiCacheFact & fact);

/** @brief 判断 role 是否有 active handler。 */
[[nodiscard]] bool hicache_fact_role_implemented(HiCacheFactRole role);

/**
 * @brief 返回 role 对应的必需 state-model 字段缺口。
 *
 * 缺口进入 summary，不触发 source result 兜底。
 */
[[nodiscard]] std::vector<std::string> hicache_required_fact_errors(const HiCacheFact & fact, HiCacheFactRole role, uint64_t effective_page_size);

} // namespace TraceGraph
