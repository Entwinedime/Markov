#pragma once

#include "trace_graph/core/dag_graph.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace TraceGraph {

/**
 * @brief 拓扑仿真的执行结果。
 *
 * e2e_ns 是 DAG critical path 长度，不是原始 trace timestamp 窗口。
 */
struct SimulationResult {
    uint64_t e2e_ns = 0;
    size_t processed_nodes = 0;
    bool cycle_detected = false;
    std::vector<size_t> cycle_nodes;
    std::string error;
};

/**
 * @brief 对已经构建好的 DAG 做拓扑重放。
 *
 * 所有边都被解释为 hard dependency：dst 的开始时间不早于 src 的完成时间。
 */
SimulationResult run_topological_simulation(DagGraph & graph);

} // namespace TraceGraph
