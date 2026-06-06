#pragma once

#include "trace_graph/core/trace_event.hpp"

#include <vector>

namespace TraceGraph {

// 在 parser 之后、DagBuilder 之前补充跨采集渠道的公共 producer/domain 字段。
// 这里不做策略推断，只做源头标记和少量类别规范化。
void normalize_trace_events(std::vector<TraceEvent> & events);

} // namespace TraceGraph
