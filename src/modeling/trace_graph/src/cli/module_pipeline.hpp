/**
 * @file
 * @brief Declares ordered construction and execution of configured model modules.
 */
#pragma once

#include "markov/trace_graph/modules/hicache/runtime/window.hpp"
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
#ifdef DEBUG
    [[nodiscard]] static ModulePipeline from_config(const std::string & filename, const std::string & hicache_oracle_cost_replay = {},
                                                    const std::string & hicache_phase_oracle_cost_replay = {},
                                                    bool hicache_canonical_observed_phase_scope = false);
#else
    [[nodiscard]] static ModulePipeline from_config(const std::string & filename);
#endif

    /** @brief Applies every owned module exactly once in configuration order. */
    void apply(core::DagGraph & graph, core::Logger & logger, const core::ClientRequestChain & chain, uint64_t begin_us, uint64_t end_us);

    [[nodiscard]] bool uses_hicache_execution() const { return execution_config_.has_value(); }
    /** @brief Static HiCache models and score-only carriers need source component observations. */
    [[nodiscard]] bool needs_hicache_observations() const;
    [[nodiscard]] const modules::hicache::runtime::HiCacheWindowResult * execution_result() const { return execution_result_ ? &*execution_result_ : nullptr; }

    /** @brief Exposes immutable modules for run-summary serialization. */
    [[nodiscard]] const std::vector<std::unique_ptr<modules::SimulationModule>> & modules() const { return modules_; }

private:
    std::vector<std::unique_ptr<modules::SimulationModule>> modules_;
    std::optional<frontend::HiCacheConfig> execution_config_;
    std::string source_prefetch_policy_;
    std::optional<modules::hicache::runtime::HiCacheWindowResult> execution_result_;
};

} // namespace markov::trace_graph::cli
