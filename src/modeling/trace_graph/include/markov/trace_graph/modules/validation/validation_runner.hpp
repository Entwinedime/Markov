/**
 * @file
 * @brief trace_graph C++ validation runner 的轻量入口。
 *
 * runner 只消费业务模块暴露的结构化状态，输出 ValidationReport；它不做文件 I/O，
 * 不读取 oracle，也不影响 DAG simulation。
 */
#pragma once

#include "markov/trace_graph/modules/module.hpp"
#include "markov/trace_graph/modules/validation/validation_report.hpp"

#include <memory>
#include <vector>

namespace markov::trace_graph::modules::validation {

/**
 * @brief 对已执行模块做轻量一致性检查。
 *
 * 当前检查只覆盖通用 wiring：模块对象存在、模块名非空，以及声明 summary 的模块
 * 确实已经进入可输出状态。领域级 oracle 对比应放在 Python validation 或未来的
 * `modules/hicache/validation` 中。
 */
[[nodiscard]] ValidationReport validate_applied_modules(const std::vector<std::unique_ptr<SimulationModule>> & modules);

} // namespace markov::trace_graph::modules::validation
