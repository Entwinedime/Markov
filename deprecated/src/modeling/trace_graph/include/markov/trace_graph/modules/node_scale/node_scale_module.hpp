/**
 * @file
 * @brief Optional framework-neutral node-duration scaling transform.
 */
#pragma once

#include "markov/trace_graph/frontend/model_config.hpp"
#include "markov/trace_graph/modules/module.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace markov::trace_graph::modules::node_scale {

/** Apply the first matching rule to an already bound duration in microseconds.
 * No match returns nullopt; a matched zero duration remains a valid zero.
 * This does not read or mutate graph structure or source observations.
 */
[[nodiscard]] std::optional<uint64_t> matched_node_duration(const frontend::NodeScaleConfig & config, std::string_view name, uint64_t duration_us);

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
