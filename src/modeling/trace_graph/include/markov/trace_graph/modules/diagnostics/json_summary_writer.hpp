/**
 * @file
 * @brief SimulationModule summary 的 diagnostics JSON 入口。
 *
 * 模块本体只暴露结构化 summary；本 writer 在 CLI diagnostics 边界把不同模块的
 * summary 转成 JSON。未知模块会输出模块名和 unsupported 标记，避免业务接口重新
 * 引入 JSON 虚函数。
 */
#pragma once

#include "markov/trace_graph/modules/module.hpp"

#include <string>

namespace markov::trace_graph::modules::diagnostics {

/** @brief 生成单个已执行模块的 summary JSON 字符串。 */
[[nodiscard]] std::string module_summary_json(const SimulationModule & module);

} // namespace markov::trace_graph::modules::diagnostics
