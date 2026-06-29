/**
 * @file
 * @brief trace_graph C++ 轻量 validation report。
 *
 * C++ validation 只做运行期结构一致性检查；HiCache oracle final-state/transition
 * 对比仍由 Python validation pipeline 负责。本层不得反向修改业务模块状态。
 */
#pragma once

#include <string>
#include <vector>

namespace markov::trace_graph::modules::validation {

/** @brief validation issue 的严重度；当前 CLI 只把 warning/error 写入 debug 日志。 */
enum class ValidationSeverity { Info, Warning, Error };

/**
 * @brief 单条 validation 发现。
 *
 * `subject` 使用模块名或具体子系统名；`message` 必须说明可行动的结构问题，
 * 不记录大段中间状态。
 */
struct ValidationIssue {
    ValidationSeverity severity = ValidationSeverity::Info;
    std::string subject;
    std::string message;
};

/**
 * @brief 一次 C++ validation pass 的结构化结果。
 *
 * `ok()` 只表示没有 Error；Warning 仍会保留，用于 Debug 构建提示 wiring 或输出
 * 边界异常。
 */
struct ValidationReport {
    std::vector<ValidationIssue> issues;

    /** @brief 当前 validation report 是否没有 error 级问题。 */
    [[nodiscard]] bool ok() const;
};

/** @brief severity 的稳定文本表示，用于日志和未来 JSON 输出。 */
[[nodiscard]] std::string validation_severity_name(ValidationSeverity severity);

} // namespace markov::trace_graph::modules::validation
