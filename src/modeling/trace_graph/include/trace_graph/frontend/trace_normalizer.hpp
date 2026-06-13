#pragma once

#include "trace_graph/core/trace_event.hpp"

#include <vector>

namespace TraceGraph {

/**
 * @brief 在 parser 之后、DagBuilder 之前补充跨采集渠道的公共 producer/domain 字段。
 *
 * normalizer 不做策略推断，只做源头标记和少量类别规范化，避免把采集来源信息
 * 和建模决策混在同一层。
 */
void normalize_trace_events(std::vector<TraceEvent> & events);

} // namespace TraceGraph
