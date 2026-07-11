/**
 * @file
 * @brief NodeScale what-if module and structured Debug result.
 */
#pragma once

#include "markov/trace_graph/frontend/model_config.hpp"
#include "markov/trace_graph/modules/module.hpp"

#include <cstdint>
#include <string>

namespace markov::trace_graph::modules::node_scale {

/**
 * @brief Structured result from the latest NodeScale execution.
 *
 * `scaled_nodes` counts matched DAG nodes. The original rule configuration is retained for
 * diagnostics, while JSON serialization remains outside the module.
 */
#ifdef DEBUG
struct NodeScaleSummary {
    frontend::NodeScaleConfig config;
    uint64_t scaled_nodes = 0;
};
#endif

/**
 * @brief Applies the first matching substring rule to each existing DAG node.
 *
 * The module scales the complete duration by the configured positive factor. It never adds,
 * removes, or rewires graph elements, making it a narrow latency-only what-if transform.
 */
class NodeScaleModule final : public modules::SimulationModule {
public:
    explicit NodeScaleModule(frontend::NodeScaleConfig config);

    /** @brief Returns the stable registry and diagnostics name. */
    [[nodiscard]] std::string_view name() const noexcept override;

    /** @brief Scales matching node durations according to the configured rule order. */
    void apply(core::DagGraph & graph) override;

#ifdef DEBUG
    /** @brief Reports whether apply() produced a diagnostics summary. */
    [[nodiscard]] bool has_summary() const override;

    /** @brief Returns the result of the most recent apply() call. */
    [[nodiscard]] NodeScaleSummary summary() const;
#endif

private:
    frontend::NodeScaleConfig config_;
#ifdef DEBUG
    uint64_t scaled_nodes_ = 0;
    bool applied_ = false;
#endif
};

} // namespace markov::trace_graph::modules::node_scale
