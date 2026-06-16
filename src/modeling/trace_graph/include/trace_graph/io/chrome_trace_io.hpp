#pragma once

#include "trace_graph/core/dag_graph.hpp"
#include "trace_graph/core/trace_event.hpp"

#include <string>
#include <vector>

namespace TraceGraph {

/**
 * @brief 读取 Chrome trace duration event。
 *
 * 当前 reader 为了处理千万级 trace 使用轻量 scanner，只抽取构图需要的字段，
 * 不保留完整 JSON DOM。
 */
[[nodiscard]] std::vector<TraceEvent> read_chrome_trace(const std::string & filename);

/**
 * @brief 以 Chrome trace 格式导出仿真后的 DAG。
 *
 * full_output=false 时只写空 traceEvents，保留文件占位；默认主输出仍是 prediction/run summary。
 */
void write_chrome_trace_dag(const std::string & filename, const DagGraph & graph, bool full_output);

} // namespace TraceGraph
