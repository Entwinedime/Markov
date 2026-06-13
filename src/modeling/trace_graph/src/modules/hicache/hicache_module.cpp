/**
 * @file
 * @brief HiCache SimulationModule 包装层。
 */
#include "trace_graph/modules/hicache/hicache_module.hpp"

#include <utility>

namespace TraceGraph {

HiCacheModule::HiCacheModule(HiCacheConfig config) : config_(std::move(config)) {}

/** @brief 返回 module registry 中使用的稳定名称。 */
std::string HiCacheModule::name() const { return "HiCacheModule"; }

/**
 * @brief 运行 HiCache 状态模型并保存 summary。
 *
 * module 包装层保持极薄：真正状态机逻辑放在 hicache_model.cpp；后续 debug 输出也应
 * 绑定 module summary/state，而不是在这里直接实现第二套逻辑。
 */
void HiCacheModule::apply(DagGraph & graph) {
    summary_ = apply_hicache_model(graph, config_);
    applied_ = true;
}

/** @brief 只有 apply 运行后才暴露 summary。 */
bool HiCacheModule::has_summary() const { return applied_; }

/** @brief 返回 SimulationModule summary JSON 外壳。 */
std::string HiCacheModule::summary_json() const { return "{\"name\":\"HiCacheModule\",\"hicache\":" + summary_.to_json() + "}"; }

} // namespace TraceGraph
