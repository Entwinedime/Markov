/**
 * @file
 * @brief Optional framework-neutral node-duration scaling transform.
 */
#pragma once

#include "markov/trace_graph/frontend/model_config.hpp"
#include "markov/trace_graph/modules/module.hpp"

#include <cstdint>
#include <string_view>

namespace markov::trace_graph::modules::node_scale {

/**
 * @brief Applies the first matching substring rule to every existing DAG node.
 *
 * NodeScale changes durations only. It does not add, remove, or rewire graph elements.
 */
class NodeScaleModule final : public modules::SimulationModule {
public:
    explicit NodeScaleModule(frontend::NodeScaleConfig config);

    [[nodiscard]] std::string_view name() const noexcept override;
    void apply(core::DagGraph & graph) override;
    [[nodiscard]] uint64_t scaled_nodes() const noexcept { return scaled_nodes_; }

private:
    frontend::NodeScaleConfig config_;
    uint64_t scaled_nodes_ = 0;
};

} // namespace markov::trace_graph::modules::node_scale
