/**
 * @file
 * @brief Debug-only DAG analysis artifact 构造器。
 */
#pragma once

#include "markov/trace_graph/core/dag_graph.hpp"

#include <string>

namespace markov::trace_graph::modules::dag_analysis {

/** @brief 阶段一 DAG analysis 输出的四份 JSON artifact。 */
struct DagAnalysisArtifacts {
    std::string dag_quality_json;
    std::string dag_analysis_json;
    std::string dag_anchor_coverage_json;
    std::string dag_operation_visibility_json;
};

/**
 * @brief 从已完成拓扑仿真的 DAG 构造阶段一 analysis artifact。
 *
 * 该函数只读 DAG，不修改 node、edge、duration 或 module state。
 */
[[nodiscard]] DagAnalysisArtifacts build_dag_analysis_artifacts(const core::DagGraph & graph);

} // namespace markov::trace_graph::modules::dag_analysis
