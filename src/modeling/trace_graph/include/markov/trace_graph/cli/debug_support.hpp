/**
 * @file
 * @brief trace_graph CLI 的 Debug/validation 边界。
 */
#pragma once

#include "markov/trace_graph/core/dag_graph.hpp"
#include "markov/trace_graph/core/logger.hpp"
#include "markov/trace_graph/modules/module.hpp"

#include <memory>
#include <map>
#include <string>
#include <vector>

namespace markov::trace_graph::cli {

/**
 * @brief 对已执行模块做 Debug-only validation。
 *
 * Release 实现为空操作；Debug 实现调用 validation runner 并写日志。
 */
void validate_modules(const std::vector<std::unique_ptr<modules::SimulationModule>> & modules, core::Logger & logger);

#ifdef DEBUG
/**
 * @brief 写出 Debug-only module summary。
 *
 * 该声明只存在于 TRACE_GRAPH_DEBUG=ON 构建，避免 release CLI 暴露 validation artifact writer。
 */
void write_module_summary(const std::string & filename, const std::vector<std::unique_ptr<modules::SimulationModule>> & modules);

/**
 * @brief 写出 Debug-only DAG analysis artifacts。
 *
 * 该输出只服务阶段一 DAG 理解和验证，不参与业务 prediction。
 */
std::map<std::string, uint64_t> write_dag_analysis_artifacts(const std::string & output_dir, const core::DagGraph & graph, size_t threads = 1);

/**
 * @brief 写出 Debug-only DAG cycle witness artifact。
 *
 * 该输出只在 validation/debug 路径使用；release 构建不暴露该 writer。
 */
void write_dag_cycle_witness_artifact(const std::string & output_dir, const core::DagGraph & graph, const std::string & error_message);
#endif

} // namespace markov::trace_graph::cli
