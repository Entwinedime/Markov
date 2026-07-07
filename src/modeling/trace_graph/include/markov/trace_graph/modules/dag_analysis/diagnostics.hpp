/**
 * @file
 * @brief Debug-only DAG analysis artifact 构造器。
 */
#pragma once

#include "markov/trace_graph/core/dag_graph.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

namespace markov::trace_graph::modules::dag_analysis {

/** @brief 阶段一 DAG analysis 输出的四份 JSON artifact。 */
struct DagAnalysisArtifacts {
    std::string dag_quality_json;
    std::string dag_analysis_json;
    std::string dag_anchor_coverage_json;
    std::string dag_operation_visibility_json;
    std::map<std::string, uint64_t> timings_ms;
};

/**
 * @brief 从已完成拓扑仿真的 DAG 构造阶段一 analysis artifact。
 *
 * 该函数只读 DAG，不修改 node、edge、duration 或 module state。
 */
[[nodiscard]] DagAnalysisArtifacts build_dag_analysis_artifacts(const core::DagGraph & graph, size_t threads = 1);

/**
 * @brief 构造 cycle failure 的结构化 witness artifact。
 *
 * 该函数用于 validation/debug 路径：当拓扑仿真发现 cycle 并失败时，输出
 * 可审计的 node/edge 证据，帮助定位是哪类边生成了闭环。
 */
[[nodiscard]] std::string build_cycle_witness_json(const core::DagGraph & graph, const std::string & error_message = "");

} // namespace markov::trace_graph::modules::dag_analysis
