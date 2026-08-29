/**
 * @file
 * @brief NodeScale duration transform implementation.
 */
#include "markov/trace_graph/modules/node_scale/node_scale_module.hpp"

#include "markov/trace_graph/core/numeric.hpp"

#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>

namespace markov::trace_graph::modules::node_scale {

NodeScaleModule::NodeScaleModule(frontend::NodeScaleConfig config) : config_(std::move(config)) {}

std::string_view NodeScaleModule::name() const noexcept { return "NodeScaleModule"; }

void NodeScaleModule::apply(core::DagGraph & graph) {
    scaled_nodes_ = 0;
    for (const auto & snapshot : graph.nodes()) {
        const auto & event = graph.event_for_node(snapshot.id);
        const auto match = std::ranges::find_if(config_.rules, [&](const auto & rule) { return event.name.contains(rule.name); });
        if (match == config_.rules.end()) continue;

        const auto scaled = core::truncate_to_u64(static_cast<double>(graph.node(snapshot.id).original_duration) * match->factor);
        if (!scaled) throw std::overflow_error("NodeScale duration exceeds uint64 range");
        graph.set_node_duration(snapshot.id, *scaled);
        (void)core::checked_increment_u64(scaled_nodes_, "NodeScale matched node count exceeds uint64 range");
    }
}

} // namespace markov::trace_graph::modules::node_scale
