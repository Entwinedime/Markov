#include "markov/templates/templates.hpp"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace markov {

std::map<std::size_t, std::size_t> TemplateOperationRewrite::insert_subgraph(const TemplateDefinition & donor, const TemplateDefinition & recipient,
                                                                             std::span<const std::size_t> donor_nodes,
                                                                             std::span<const std::size_t> donor_dependencies,
                                                                             std::map<std::size_t, std::size_t> boundary_mapping,
                                                                             const std::map<std::size_t, std::size_t> & lane_mapping) {
    std::set<std::size_t> referenced;
    for (const auto index : donor_nodes) {
        referenced.insert(donor.graph.at(index).begin);
        referenced.insert(donor.graph.at(index).end);
    }
    for (const auto index : donor_dependencies) {
        referenced.insert(donor.graph.dependencies().at(index).source);
        referenced.insert(donor.graph.dependencies().at(index).target);
    }
    for (const auto index : referenced) {
        if (boundary_mapping.contains(index)) continue;
        auto point = donor.graph.boundary(index);
        point.lane = lane_mapping.at(point.lane);
        boundary_mapping.emplace(index, recipient.graph.boundary_count() + inserted_boundaries.size());
        inserted_boundaries.push_back({ point, donor.boundary_observations.at(index).source_boundary, &donor.source_manifest, donor.cpu_boundary(index) });
    }

    for (const auto index : donor_nodes) {
        auto node = donor.graph.at(index);
        node.begin = boundary_mapping.at(node.begin);
        node.end = boundary_mapping.at(node.end);
        inserted_nodes.push_back({ node, &donor });

        if (node.kind != SourceSpanKind::HostWait) continue;
        const auto found = std::ranges::find(donor.stream_waits, donor.observation(node).source_span, &TemplateStreamWait::source_span);
        if (found == donor.stream_waits.end()) continue;

        auto & wait = inserted_nodes.back().stream_wait.emplace(*found);
        wait.completion = node.end;
        for (auto & resource : wait.resources) {
            for (const auto & [from, to] : lane_mapping) {
                const auto & source_lane = donor.lanes[from].resource();
                if (resource != source_lane) continue;
                const auto & target_lane = (to < recipient.lanes.size() ? recipient.lanes[to] : inserted_lanes.at(to - recipient.lanes.size())).resource();
                resource = target_lane;
                break;
            }
        }
    }

    for (const auto index : donor_dependencies) {
        const auto & edge = donor.graph.dependencies().at(index);
        added_dependencies.push_back({ edge.relation, boundary_mapping.at(edge.source), boundary_mapping.at(edge.target) });
    }
    return boundary_mapping;
}

const TemplateNodeObservation & TemplateInsertedNode::observation() const { return definition->observation(node); }

std::vector<std::vector<std::size_t>> TemplateTopology::successors(const TemplateDefinition & definition) const {
    std::vector<std::vector<std::size_t>> result(source_boundary_count_ + inserted_boundaries_.size());
    for (const auto index : dependencies_) {
        const auto edge = definition.graph.dependencies()[index];
        result[edge.source].push_back(edge.target);
    }
    for (const auto & edge : added_dependencies_) result[edge.source].push_back(edge.target);
    for (const auto index : nodes_) {
        const auto node = definition.graph[index];
        if (node.begin != node.end) result[node.begin].push_back(node.end);
    }
    for (const auto & inserted : inserted_nodes_)
        if (inserted.node.begin != inserted.node.end) result[inserted.node.begin].push_back(inserted.node.end);
    return result;
}

TemplateTopology::TemplateTopology(const TemplateDefinition & definition)
    : rotary_dependencies_(definition.rotary_position_dependencies),
      source_boundary_count_(definition.graph.boundary_count()),
      source_lane_count_(definition.lanes.size()) {
    for (std::size_t index = 0; index < definition.graph.size(); ++index) nodes_.push_back(index);
    for (std::size_t index = 0; index < definition.graph.dependencies().size(); ++index) dependencies_.push_back(index);
}

std::expected<std::vector<std::size_t>, std::string> TemplateTopology::apply_operations(const TemplateDefinition & definition,
                                                                                        std::span<const TemplateOperationRewrite> removals) {
    std::set<std::size_t> removed_nodes;
    std::set<std::size_t> removed_dependencies;
    std::map<std::size_t, std::size_t> producers;
    for (const auto & removal : removals) {
        removed_nodes.insert(removal.nodes.begin(), removal.nodes.end());
        removed_dependencies.insert(removal.dependencies.begin(), removal.dependencies.end());
        for (const auto index : removal.rotary_dependencies) {
            const auto [position, inserted] = producers.emplace(index, removal.rotary_producer.value());
            if (!inserted && position->second != removal.rotary_producer.value()) return std::unexpected("operation_rotary_replacement_conflict");
        }
    }

    // Stage the complete union, including adjacent operations, before committing.
    // A rejected replacement leaves this instance and its shared source untouched.
    auto replacement = *this;
    std::erase_if(replacement.nodes_, [&removed_nodes](auto index) { return removed_nodes.contains(index); });
    std::erase_if(replacement.dependencies_, [&removed_dependencies](auto index) { return removed_dependencies.contains(index); });
    for (const auto & [index, producer] : producers) replacement.rotary_dependencies_.at(index).producer = producer;
    for (const auto & dependency : replacement.rotary_dependencies_) {
        if (!std::ranges::binary_search(replacement.nodes_, dependency.producer) || !std::ranges::binary_search(replacement.nodes_, dependency.consumer))
            return std::unexpected("operation_removal_leaves_missing_data_endpoint");
    }
    std::vector<std::size_t> insertion_offsets;
    struct ResourceRegion {
        TemplateRegionPort port;
        std::optional<std::size_t> region;
        std::size_t first_node;
        std::size_t last_node;
    };
    std::map<TemplateResource, std::vector<ResourceRegion>> resource_regions;
    for (const auto & port : replacement.added_resource_ports_) resource_regions[port.resource].push_back({ port, std::nullopt, 0, 0 });
    for (const auto & removal : removals) {
        // Plans use their own insertion offsets. Relocate each plan into the
        // instance before combining it with other independent replacements.
        const auto offset = replacement.inserted_boundaries_.size();
        insertion_offsets.push_back(offset);
        for (auto edge : removal.added_dependencies) {
            if (edge.source >= source_boundary_count_) edge.source += offset;
            if (edge.target >= source_boundary_count_) edge.target += offset;
            replacement.added_dependencies_.push_back(edge);
        }
        const auto first_node = replacement.inserted_nodes_.size();
        for (auto inserted : removal.inserted_nodes) {
            auto & node = inserted.node;
            if (node.begin >= source_boundary_count_) node.begin += offset;
            if (node.end >= source_boundary_count_) node.end += offset;
            if (inserted.stream_wait) inserted.stream_wait->completion = node.end;
            replacement.inserted_nodes_.push_back(std::move(inserted));
        }
        const auto lane_offset = replacement.inserted_lanes_.size();
        for (auto boundary : removal.inserted_boundaries) {
            if (boundary.boundary.lane >= source_lane_count_) boundary.boundary.lane += lane_offset;
            replacement.inserted_boundaries_.push_back(boundary);
        }
        replacement.inserted_lanes_.insert(replacement.inserted_lanes_.end(), removal.inserted_lanes.begin(), removal.inserted_lanes.end());
        for (auto port : removal.added_resource_ports) {
            if (port.entry >= source_boundary_count_) port.entry += offset;
            if (port.exit >= source_boundary_count_) port.exit += offset;
            if (port.device_done && *port.device_done >= source_boundary_count_) *port.device_done += offset;
            replacement.added_resource_ports_.push_back(port);
            resource_regions[port.resource].push_back({ port, removal.source_region, first_node, replacement.inserted_nodes_.size() });
        }
    }

    // Link only with proven source-region order. Planning order and donor
    // timestamps never define serialization of newly shared target resources.
    for (const auto & [resource, uses] : resource_regions) {
        for (std::size_t left = 0; left < uses.size(); ++left) {
            for (std::size_t right = left + 1; right < uses.size(); ++right) {
                const auto & a = uses[left];
                const auto & b = uses[right];
                if (!a.region || !b.region) return std::unexpected("region_new_resource_order_not_qualified");
                const auto order = definition.region_order(definition.regions[*a.region], definition.regions[*b.region]);
                if (order == std::partial_ordering::unordered) return std::unexpected("region_new_resource_order_not_qualified");
                const auto & first = order < 0 ? a : b;
                const auto & second = order < 0 ? b : a;
                const auto relation = (resource).domain == SourceDomain::CpuThread ? SourceRelation::ThreadOrder : SourceRelation::StreamOrder;
                replacement.added_dependencies_.push_back({ relation, first.port.exit, second.port.entry });

                // A query in the later inserted region sees earlier inserted
                // submissions too, in addition to the preceding batch frontier.
                for (auto node = second.first_node; node < second.last_node; ++node) {
                    const auto & wait = replacement.inserted_nodes_[node].stream_wait;
                    if (!wait || !wait->has_resource_scope() || std::ranges::find(wait->resources, resource) == wait->resources.end()) continue;
                    replacement.added_dependencies_.push_back(
                        { SourceRelation::NativeHostWait, first.port.device_done.value_or(first.port.exit), wait->completion });
                }
            }
        }
    }

    *this = std::move(replacement);
    return insertion_offsets;
}

void TemplateTopology::bind_inserted_kernel(std::size_t index, SourceKernel kernel) { inserted_nodes_.at(index).kernel_override = std::move(kernel); }

void TemplateTopology::bind_inserted_transfer(std::size_t index, SourceTransfer transfer) { inserted_nodes_.at(index).transfer_override = transfer; }

} // namespace markov
