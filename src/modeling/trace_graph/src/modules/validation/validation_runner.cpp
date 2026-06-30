/**
 * @file
 * @brief trace_graph C++ validation runner 实现。
 */
#include "markov/trace_graph/modules/validation/validation_runner.hpp"

#include <algorithm>
#include <ranges>

namespace markov::trace_graph::modules::validation {

bool ValidationReport::ok() const {
    return std::ranges::none_of(issues, [](const auto & issue) { return issue.severity == ValidationSeverity::Error; });
}

/** @brief validation severity 名称是 summary/CLI 里使用的稳定英文枚举值。 */
std::string validation_severity_name(ValidationSeverity severity) {
    switch (severity) {
    case ValidationSeverity::Info:
        return "info";
    case ValidationSeverity::Warning:
        return "warning";
    case ValidationSeverity::Error:
        return "error";
    }
    return "unknown";
}

/**
 * @brief 检查已经构造出的模块列表是否满足最小执行合同。
 *
 * validation runner 不重新解释 model config，只验证 CLI 即将执行的模块对象是否可用。
 */
ValidationReport validate_applied_modules(const std::vector<std::unique_ptr<SimulationModule>> & modules) {
    ValidationReport report;
    for (const auto & module : modules) {
        if (!module) {
            report.issues.push_back({
                .severity = ValidationSeverity::Error,
                .subject = "module_registry",
                .message = "module list contains null entry",
            });
            continue;
        }
        const auto module_name = module->name();
        if (module_name.empty()) {
            report.issues.push_back({
                .severity = ValidationSeverity::Error,
                .subject = "module_registry",
                .message = "module name is empty",
            });
        }
    }
    return report;
}

} // namespace markov::trace_graph::modules::validation
