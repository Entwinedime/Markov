/**
 * @file
 * @brief Debug-only TraceGraph CLI diagnostics implementation.
 */
#include "markov/trace_graph/cli/debug_support.hpp"

#include "file_output.hpp"

#include "markov/trace_graph/modules/dag_analysis/diagnostics.hpp"
#include "markov/trace_graph/modules/diagnostics/json_summary_writer.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <ranges>

namespace markov::trace_graph::cli {

namespace {

using Json = nlohmann::json;

} // namespace

void write_module_summary(const std::string & filename, const std::vector<std::unique_ptr<modules::SimulationModule>> & modules) {
    Json root;
    root["modules"] = Json::array();
    std::ranges::for_each(modules, [&](const auto & module) {
        if (!module || !module->has_summary()) return;
        root["modules"].push_back(Json::parse(modules::diagnostics::module_summary_json(*module)));
    });
    write_json_file(filename, root);
}

std::map<std::string, uint64_t> write_dag_analysis_artifacts(const std::string & output_dir, const core::DagGraph & graph, size_t threads) {
    std::filesystem::create_directories(output_dir);
    const auto artifacts = modules::dag_analysis::build_dag_analysis_artifacts(graph, threads);
    write_text_file((std::filesystem::path(output_dir) / "dag_quality.json").string(), artifacts.dag_quality_json);
    write_text_file((std::filesystem::path(output_dir) / "dag_analysis.json").string(), artifacts.dag_analysis_json);
    write_text_file((std::filesystem::path(output_dir) / "dag_anchor_coverage.json").string(), artifacts.dag_anchor_coverage_json);
    write_text_file((std::filesystem::path(output_dir) / "dag_operation_visibility.json").string(), artifacts.dag_operation_visibility_json);
    return artifacts.timings_ms;
}

void write_dag_failure_artifact(const std::string & output_dir, const core::DagGraph & graph, const std::exception & error, core::Logger & logger) {
    try {
        if (const auto * mutation_error = dynamic_cast<const core::DagMutationValidationError *>(&error)) {
            write_text_file((std::filesystem::path(output_dir) / "dag_topology_validation.json").string(),
                            modules::dag_analysis::build_topology_validation_json(graph, mutation_error->report(), mutation_error->what()));
        }
        else {
            write_text_file((std::filesystem::path(output_dir) / "dag_cycle_witness.json").string(),
                            modules::dag_analysis::build_cycle_witness_json(graph, error.what()));
        }
    }
    catch (const std::exception & artifact_error) {
        logger.warn() << "Failed to write DAG failure artifact: " << artifact_error.what();
    }
}

} // namespace markov::trace_graph::cli
