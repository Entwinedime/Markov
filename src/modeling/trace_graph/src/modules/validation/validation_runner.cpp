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
