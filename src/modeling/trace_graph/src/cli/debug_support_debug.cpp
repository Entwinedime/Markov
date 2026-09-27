/**
 * @file
 * @brief Debug-only TraceGraph CLI diagnostics implementation.
 */
#include "markov/trace_graph/cli/debug_support.hpp"

#include "file_output.hpp"

#include "markov/trace_graph/modules/diagnostics/json_summary_writer.hpp"

#include <nlohmann/json.hpp>

namespace markov::trace_graph::cli {

namespace {

using Json = nlohmann::json;

} // namespace

void write_module_summary(const std::string & filename, const std::vector<std::unique_ptr<modules::SimulationModule>> & modules) {
    Json root;
    root["modules"] = Json::array();
    for (const auto & module : modules) {
        if (module && module->has_summary()) root["modules"].push_back(modules::diagnostics::module_summary_json(*module));
    }
    write_json_file(filename, root);
}

} // namespace markov::trace_graph::cli
