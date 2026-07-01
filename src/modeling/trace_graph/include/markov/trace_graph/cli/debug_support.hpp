/**
 * @file
 * @brief trace_graph CLI 的 Debug/validation 边界。
 */
#pragma once

#include "markov/trace_graph/core/logger.hpp"
#include "markov/trace_graph/modules/module.hpp"

#include <memory>
#include <string>
#include <vector>

namespace markov::trace_graph::cli {

/**
 * @brief 对已执行模块做 Debug-only validation。
 *
 * Release 实现为空操作；Debug 实现调用 validation runner 并写日志。
 */
void validate_modules(const std::vector<std::unique_ptr<modules::SimulationModule>> & modules, core::Logger & logger);

/**
 * @brief 写出 Debug-only module summary。
 *
 * Release 实现直接报错，避免默认业务路径链接 diagnostics writer。
 */
void write_module_summary(const std::string & filename, const std::vector<std::unique_ptr<modules::SimulationModule>> & modules);

} // namespace markov::trace_graph::cli
