/**
 * @file
 * @brief trace_graph CLI Release stub for Debug/validation support.
 */
#include "markov/trace_graph/cli/debug_support.hpp"

#include <stdexcept>

namespace markov::trace_graph::cli {

void validate_modules(const std::vector<std::unique_ptr<modules::SimulationModule>> & /*modules*/, core::Logger & /*logger*/) {}

void write_module_summary(const std::string & /*filename*/, const std::vector<std::unique_ptr<modules::SimulationModule>> & /*modules*/) {
    throw std::runtime_error("--model-summary requires TRACE_GRAPH_DEBUG=ON");
}

} // namespace markov::trace_graph::cli
