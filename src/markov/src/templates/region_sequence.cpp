#include "markov/templates/templates.hpp"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace markov {


std::expected<TemplateSubgraphRegion, std::string> TemplateDefinition::cpu_sequence_separator(std::size_t entry, std::size_t exit, std::size_t parent) const {
    const auto lane = graph.boundary(entry).lane;
    if (graph.boundary(exit).lane != lane || lanes[lane].source.domain != SourceDomain::CpuThread)
        return std::unexpected("request_sequence_separator_resource_not_qualified");

    // The cut is the native CPU path between member ports, not the interval
    // between their timestamps. Preserve every observed service on that path.
    std::map<std::size_t, std::vector<std::size_t>> successors, predecessors;
    for (const auto & edge : graph.dependencies()) {
        if (edge.relation != SourceRelation::ThreadOrder && edge.relation != SourceRelation::IntervalExtent) continue;
        if (graph.boundary(edge.source).lane != lane || graph.boundary(edge.target).lane != lane) continue;
        successors[edge.source].push_back(edge.target);
        predecessors[edge.target].push_back(edge.source);
    }
    std::set<std::size_t> reached{ entry }, ancestors{ exit };
    std::vector<std::size_t> pending{ entry };
    for (std::size_t cursor = 0; cursor < pending.size(); ++cursor) {
        for (const auto point : successors[pending[cursor]])
            if (reached.insert(point).second) pending.push_back(point);
    }
    if (!reached.contains(exit)) return std::unexpected("request_sequence_separator_not_observed");

    pending = { exit };
    for (std::size_t cursor = 0; cursor < pending.size(); ++cursor) {
        for (const auto point : predecessors[pending[cursor]])
            if (ancestors.insert(point).second) pending.push_back(point);
    }
    TemplateSubgraphRegion cut;
    for (const auto point : reached)
        if (ancestors.contains(point)) cut.boundaries.push_back(point);

    bool has_service = false;
    for (std::size_t index = 0; index < graph.size(); ++index) {
        const auto node = graph[index];
        const bool begin = std::ranges::binary_search(cut.boundaries, node.begin);
        const bool end = std::ranges::binary_search(cut.boundaries, node.end);
        if (begin && end) {
            if (node.role() != TemplateNodeRole::Annotation) {
                if (node.kind != SourceSpanKind::CpuControl || observation(node).source_scope.value_or(source_call) != parent)
                    return std::unexpected("request_sequence_separator_work_not_qualified");
                has_service = true;
            }
            cut.nodes.push_back(index);
        }
        else if (node.role() != TemplateNodeRole::Annotation && ((begin && node.begin != exit) || (end && node.end != entry))) {
            cut.crossing_nodes.push_back(index);
        }
    }
    if (!has_service) return std::unexpected("request_sequence_separator_not_observed");

    // External events, retained work and additional resources must remain
    // visible to the same port qualification used for the member bodies.
    cut = complete_region(std::move(cut));
    const auto ports = region_ports(cut);
    if (!ports) return std::unexpected(ports.error());
    if (ports->size() != 1 || ports->front().resource != lanes[lane].resource() || ports->front().entry != entry || ports->front().exit != exit)
        return std::unexpected("request_sequence_separator_interface_not_qualified");
    return cut;
}

std::expected<TemplateOperationRewrite, std::string> TemplateDefinition::plan_region_sequence(std::span<const std::size_t> source_members,
                                                                                              std::span<const TemplateRegionReference> members) const {
    if (source_members.empty() || members.empty()) return std::unexpected("request_sequence_empty");
    for (const auto & member : members)
        if (source_manifest != member.definition->source_manifest && (!has_resource_roles() || !member.definition->has_resource_roles()))
            return std::unexpected("cross_source_resource_role_not_qualified");

    // A one-member sequence is exactly the existing region replacement. It
    // retains that path's resource insertion and neighboring wait qualification.
    if (source_members.size() == 1 && members.size() == 1) {
        auto result =
            plan_region_replacement(regions[source_members.front()], *members.front().definition, members.front().definition->regions[members.front().region]);
        if (!result) return std::unexpected(result.error());
        for (auto & inserted : result->inserted_nodes) inserted.request_slot = 0;
        return result;
    }

    TemplateSubgraphRegion sequence;
    struct Separator {
        TemplateSubgraphRegion region;
        std::size_t entry;
        std::size_t exit;
    };
    std::vector<Separator> separators;
    std::optional<std::size_t> previous_exit;
    const auto parent = regions[source_members.front()].parent_call;
    const auto & host_lane = lanes[graph.boundary(entry).lane].resource();
    const TemplateResource host_resource = host_lane;
    for (const auto index : source_members) {
        const auto & member = regions[index];
        if (member.parent_call != parent) return std::unexpected("request_sequence_member_not_qualified");
        const auto member_ports = region_ports(semantic_region(member));
        if (!member_ports) return std::unexpected(member_ports.error());
        const auto host = std::ranges::find(*member_ports, host_resource, &TemplateRegionPort::resource);
        if (host == member_ports->end()) return std::unexpected("request_sequence_host_not_qualified");

        if (previous_exit) {
            auto separator = cpu_sequence_separator(*previous_exit, host->entry, parent);
            if (!separator) return std::unexpected(separator.error());
            sequence.nodes.insert(sequence.nodes.end(), separator->nodes.begin(), separator->nodes.end());
            sequence.boundaries.insert(sequence.boundaries.end(), separator->boundaries.begin(), separator->boundaries.end());
            separators.push_back({ std::move(*separator), *previous_exit, host->entry });
        }
        sequence.nodes.insert(sequence.nodes.end(), member.nodes.begin(), member.nodes.end());
        previous_exit = host->exit;
    }
    std::ranges::sort(sequence.nodes);
    if (std::ranges::adjacent_find(sequence.nodes) != sequence.nodes.end()) return std::unexpected("request_sequence_members_overlap");

    std::set<std::size_t> points(sequence.boundaries.begin(), sequence.boundaries.end());
    for (const auto index : sequence.nodes) {
        points.insert(graph[index].begin);
        points.insert(graph[index].end);
    }
    sequence.boundaries.assign(points.begin(), points.end());
    sequence = complete_region(std::move(sequence));
    const auto ports = region_ports(sequence);
    if (!ports) return std::unexpected(ports.error());
    const auto host = std::ranges::find(*ports, host_resource, &TemplateRegionPort::resource);
    if (host == ports->end()) return std::unexpected("request_sequence_host_not_qualified");
    if (members.size() > 1 && separators.empty()) return std::unexpected("request_sequence_separator_not_observed");

    const auto & port = *host;
    const auto lane = graph.boundary(port.entry).lane;
    TemplateOperationRewrite result;
    result.nodes = sequence.nodes;
    result.dependencies = sequence.internal_dependencies;
    for (const auto index : sequence.incoming_dependencies) {
        if (is_region_wait_import(graph.dependencies()[index], sequence)) result.dependencies.push_back(index);
    }

    // Resource order and device completion are separate frontiers. The first
    // stream entry is an empty boundary fed by the preserved outside producer.
    std::map<TemplateResource, std::size_t> frontiers;
    std::map<TemplateResource, std::size_t> completions;
    for (const auto & resource : *ports) {
        frontiers.emplace(resource.resource, resource.entry);
        if ((resource.resource).domain == SourceDomain::DeviceStream) completions.emplace(resource.resource, resource.entry);
    }
    auto entry = port.entry;
    for (std::size_t slot = 0; slot < members.size(); ++slot) {
        const auto & donor = *members[slot].definition;
        const auto & region = donor.regions[members[slot].region];
        if (donor.rank != rank || donor.step != step || donor.phase != phase) return std::unexpected("request_sequence_donor_not_qualified");
        const auto cut = donor.semantic_region(region);
        const auto donor_ports = donor.region_ports(cut);
        if (!donor_ports) return std::unexpected(donor_ports.error());
        const auto donor_host = std::ranges::find(*donor_ports, host_resource, &TemplateRegionPort::resource);
        if (donor_host == donor_ports->end()) return std::unexpected("request_sequence_host_not_qualified");

        const bool last = slot + 1 == members.size();
        auto exit = port.exit;
        if (!last) {
            exit = graph.boundary_count() + result.inserted_boundaries.size();
            result.inserted_boundaries.push_back({ { lane }, donor.boundary_observations[donor_host->exit].source_boundary, &donor.source_manifest });
        }
        std::map<std::size_t, std::size_t> mapping{
            { donor_host->entry, entry },
            {  donor_host->exit,  exit }
        };
        std::map<std::size_t, std::size_t> lane_mapping;
        std::optional<std::size_t> completion_lane;
        for (const auto & from : *donor_ports) {
            const auto to = std::ranges::find(*ports, from.resource, &TemplateRegionPort::resource);
            if (to == ports->end()) return std::unexpected("request_sequence_additional_resource_not_supported");
            lane_mapping.emplace(donor.graph.boundary(from.entry).lane, graph.boundary(to->entry).lane);
            lane_mapping.emplace(donor.graph.boundary(from.exit).lane, graph.boundary(to->exit).lane);
            if ((from.resource).domain == SourceDomain::DeviceStream) {
                if (completion_lane) return std::unexpected("request_sequence_multiple_streams_not_qualified");
                completion_lane = graph.boundary(to->exit).lane;
            }
        }
        for (const auto point : cut.boundaries) {
            const auto from_lane = donor.graph.boundary(point).lane;
            if (lane_mapping.contains(from_lane)) continue;
            if (!completion_lane || donor.lanes[from_lane].source.domain != SourceDomain::DeviceTaskCompletion)
                return std::unexpected("request_sequence_additional_resource_not_supported");
            lane_mapping.emplace(from_lane, *completion_lane);
        }

        const auto inserted_begin = result.inserted_nodes.size();
        mapping = result.insert_subgraph(donor, *this, cut.nodes, cut.internal_dependencies, std::move(mapping), lane_mapping);
        // Query dependencies are bound before advancing the member's resource
        // frontiers, so a wait cannot accidentally depend on its own new work.
        for (const auto index : cut.incoming_dependencies) {
            const auto & edge = donor.graph.dependencies()[index];
            if (!donor.is_region_wait_import(edge, cut)) continue;
            const auto & source_lane = donor.lanes[donor.graph.boundary(edge.source).lane].resource();
            const TemplateResource resource = source_lane;
            const auto predecessor = completions.find(resource);
            if (predecessor == completions.end()) return std::unexpected("request_sequence_wait_resource_not_exposed");
            result.added_dependencies.push_back({ SourceRelation::NativeHostWait, predecessor->second, mapping.at(edge.target) });
        }
        for (const auto & from : *donor_ports) {
            if (from.resource == host_resource) continue;
            const auto relation = (from.resource).domain == SourceDomain::CpuThread ? SourceRelation::ThreadOrder : SourceRelation::StreamOrder;
            result.added_dependencies.push_back({ relation, frontiers.at(from.resource), mapping.at(from.entry) });
            frontiers.at(from.resource) = mapping.at(from.exit);
            if ((from.resource).domain == SourceDomain::DeviceStream) completions.at(from.resource) = mapping.at(from.device_done.value_or(from.exit));
        }
        for (auto index = inserted_begin; index < result.inserted_nodes.size(); ++index) result.inserted_nodes[index].request_slot = slot;

        if (last) break;

        // Keep parent work between members, including annotations and internal
        // boundaries; a direct edge would discard their required causality.
        const auto & bridge = separators[std::min(slot, separators.size() - 1)];
        entry = graph.boundary_count() + result.inserted_boundaries.size();
        result.inserted_boundaries.push_back({ { lane }, boundary_observations[bridge.exit].source_boundary, &source_manifest });
        result.insert_subgraph(*this,
                               *this,
                               bridge.region.nodes,
                               bridge.region.internal_dependencies,
                               {
                                   { bridge.entry,  exit },
                                   {  bridge.exit, entry }
        },
                               { { graph.boundary(bridge.entry).lane, lane } });
    }
    // Preserve the sequence's exported identities even when its final member
    // has no work on a resource used by an earlier member.
    for (const auto & resource : *ports) {
        if (resource.resource == host_resource) continue;
        const auto relation = (resource.resource).domain == SourceDomain::CpuThread ? SourceRelation::ThreadOrder : SourceRelation::StreamOrder;
        result.added_dependencies.push_back({ relation, frontiers.at(resource.resource), resource.exit });
        if (resource.device_done)
            result.added_dependencies.push_back({ SourceRelation::CompletionJoin, completions.at(resource.resource), *resource.device_done });
    }
    return result;
}

} // namespace markov
