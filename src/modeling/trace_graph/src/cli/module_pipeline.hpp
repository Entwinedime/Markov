/**
 * @file
 * @brief Declares ordered construction and execution of configured model modules.
 */
#pragma once

#include "markov/trace_graph/modules/module.hpp"

#include <memory>
#include <string>
#include <vector>

namespace markov::trace_graph::core {
class Logger;
}

namespace markov::trace_graph::cli {

/**
 * @brief Owns configured modules in their semantic application order.
 *
 * The pipeline is built before trace loading so invalid model configuration fails
 * without paying trace parse or DAG construction cost.
 */
class ModulePipeline {
public:
    /** @brief Parses one C++ model config; an empty path creates an empty pipeline. */
    [[nodiscard]] static ModulePipeline from_config(const std::string & filename);

    /** @brief Applies every owned module exactly once in configuration order. */
    void apply(core::DagGraph & graph, core::Logger & logger) const;

    /** @brief Exposes immutable modules for run-summary serialization. */
    [[nodiscard]] const std::vector<std::unique_ptr<modules::SimulationModule>> & modules() const { return modules_; }

private:
    std::vector<std::unique_ptr<modules::SimulationModule>> modules_;
};

} // namespace markov::trace_graph::cli
