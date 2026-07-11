/**
 * @file
 * @brief Duration-only NodeScaleModule implementation.
 */
#include "markov/trace_graph/modules/node_scale/node_scale_module.hpp"

#include "markov/trace_graph/core/numeric.hpp"

#include <algorithm>
#include <cstdint>
#include <ranges>
#include <string>
#include <utility>

namespace markov::trace_graph::modules::node_scale {

namespace node_scale_detail {

const frontend::NodeScaleRuleConfig * find_matching_rule(const std::vector<frontend::NodeScaleRuleConfig> & rules, const std::string & name) {
    const auto match = std::ranges::find_if(rules, [&](const auto & rule) { return !rule.name.empty() && name.contains(rule.name); });
    return match == rules.end() ? nullptr : &*match;
}

} // namespace node_scale_detail

using node_scale_detail::find_matching_rule;

NodeScaleModule::NodeScaleModule(frontend::NodeScaleConfig config) : config_(std::move(config)) {}

std::string_view NodeScaleModule::name() const noexcept { return "NodeScaleModule"; }

void NodeScaleModule::apply(core::DagGraph & graph) {
#ifdef DEBUG
    applied_ = true;
    scaled_nodes_ = 0;
#endif
    // Node storage remains stable because this module changes durations only.
    for (const auto & node_snapshot : graph.nodes()) {
        auto node_id = node_snapshot.id;
        const auto & record = graph.event_for_node(node_id);
        // Rule order is significant: the first non-empty substring match wins.
        const auto * rule = find_matching_rule(config_.rules, record.name);
        if (rule == nullptr) continue;

        const uint64_t original_time = graph.node(node_id).original_duration;

        const auto scaled = core::truncate_to_u64(static_cast<double>(original_time) * rule->factor);
        if (!scaled) throw std::overflow_error("NodeScale duration exceeds uint64 range");

        graph.set_node_duration(node_id, *scaled);
#ifdef DEBUG
        (void)core::checked_increment_u64(scaled_nodes_, "NodeScale matched node count exceeds uint64 range");
#endif
    }
}

#ifdef DEBUG
bool NodeScaleModule::has_summary() const { return applied_; }

NodeScaleSummary NodeScaleModule::summary() const { return NodeScaleSummary{ .config = config_, .scaled_nodes = scaled_nodes_ }; }
#endif

} // namespace markov::trace_graph::modules::node_scale
