/**
 * @file
 * @brief Debug-only TraceGraph CLI diagnostics implementation.
 */
#include "markov/trace_graph/cli/debug_support.hpp"

#include "file_output.hpp"

#include "markov/trace_graph/modules/diagnostics/json_summary_writer.hpp"

#include <nlohmann/json.hpp>

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
        auto summary = Json::parse(modules::diagnostics::module_summary_json(*module));
        root["modules"].push_back(std::move(summary));
    });
    write_json_file(filename, root);
}

} // namespace markov::trace_graph::cli
