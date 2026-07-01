/**
 * @file
 * @brief HiCache model summary 的 diagnostics JSON writer。
 *
 * 这里不定义模型状态，只把 `model::HiCacheSummary` 序列化为 workflow 需要的
 * model_summary JSON。状态机核心不得包含本头文件。
 */
#pragma once

#include "markov/trace_graph/modules/hicache/model/summary.hpp"

#include <string>

namespace markov::trace_graph::modules::hicache::diagnostics {

/**
 * @brief 把 HiCache 结构化模型结果写成 JSON 字符串。
 *
 * 本函数只负责 Debug/validation diagnostics 表达，不改变 summary 内容。
 */
[[nodiscard]] std::string summary_json(const model::HiCacheSummary & summary);

} // namespace markov::trace_graph::modules::hicache::diagnostics
