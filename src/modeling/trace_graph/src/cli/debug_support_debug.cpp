/**
 * @file
 * @brief trace_graph CLI Debug/validation 支持实现。
 */
#include "markov/trace_graph/cli/debug_support.hpp"

#include "markov/trace_graph/modules/dag_analysis/diagnostics.hpp"
#include "markov/trace_graph/modules/diagnostics/json_summary_writer.hpp"
#include "markov/trace_graph/modules/validation/validation_runner.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <ranges>
#include <stdexcept>

namespace markov::trace_graph::cli {

namespace {

using Json = nlohmann::json;

void write_json_file(const std::string & filename, const Json & value) {
    std::ofstream ofs(filename);
    if (!ofs.is_open()) { throw std::runtime_error("Failed to write JSON file: " + filename); }
    ofs << value.dump(2) << "\n";
}

void write_text_file(const std::string & filename, const std::string & value) {
    std::ofstream ofs(filename);
    if (!ofs.is_open()) { throw std::runtime_error("Failed to write JSON file: " + filename); }
    ofs << value << "\n";
}

} // namespace

void validate_modules(const std::vector<std::unique_ptr<modules::SimulationModule>> & modules, core::Logger & logger) {
    const auto module_validation = modules::validation::validate_applied_modules(modules);
    for (const auto & issue : module_validation.issues) {
        logger.warn() << "validation/" << modules::validation::validation_severity_name(issue.severity) << " " << issue.subject << ": " << issue.message;
    }
}

void write_module_summary(const std::string & filename, const std::vector<std::unique_ptr<modules::SimulationModule>> & modules) {
    Json root;
    root["modules"] = Json::array();
    std::ranges::for_each(modules, [&](const auto & module) {
        if (!module || !module->has_summary()) return;
        root["modules"].push_back(Json::parse(modules::diagnostics::module_summary_json(*module)));
    });
    write_json_file(filename, root);
}

void write_dag_analysis_artifacts(const std::string & output_dir, const core::DagGraph & graph) {
    std::filesystem::create_directories(output_dir);
    const auto artifacts = modules::dag_analysis::build_dag_analysis_artifacts(graph);
    write_text_file((std::filesystem::path(output_dir) / "dag_quality.json").string(), artifacts.dag_quality_json);
    write_text_file((std::filesystem::path(output_dir) / "dag_analysis.json").string(), artifacts.dag_analysis_json);
    write_text_file((std::filesystem::path(output_dir) / "dag_anchor_coverage.json").string(), artifacts.dag_anchor_coverage_json);
    write_text_file((std::filesystem::path(output_dir) / "dag_operation_visibility.json").string(), artifacts.dag_operation_visibility_json);
}

} // namespace markov::trace_graph::cli
