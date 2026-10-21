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

TemplateSemanticPath rebase_semantic_path(const TemplateSemanticPath & path, std::size_t prefix_size, std::span<const std::string_view> target_prefix) {
    if (!path || prefix_size > path->size()) return {};
    std::vector<std::string_view> target(target_prefix.begin(), target_prefix.end());
    target.insert(target.end(), path->begin() + prefix_size, path->end());
    return std::make_shared<const std::vector<std::string_view>>(std::move(target));
}

std::map<std::size_t, std::size_t> TemplateOperationRewrite::insert_subgraph(const TemplateDefinition & donor, const TemplateDefinition & recipient,
                                                                             std::span<const std::size_t> donor_nodes,
                                                                             std::span<const std::size_t> donor_dependencies,
                                                                             std::map<std::size_t, std::size_t> boundary_mapping,
                                                                             const std::map<std::size_t, std::size_t> & lane_mapping,
                                                                             std::optional<std::pair<std::size_t, std::size_t>> scope_roots) {
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
        auto point = donor.graph.boundary(index);
        const auto domain = donor.lanes[point.lane].source.domain;
        const auto mapped = boundary_mapping.find(index);
        if (mapped != boundary_mapping.end() && domain != SourceDomain::CpuThread) continue;
        point.lane = lane_mapping.at(point.lane);
        TemplateInsertedBoundary observation{ point, donor.boundary_observations.at(index).source_boundary, &donor.source_manifest, donor.cpu_boundary(index) };
        if (mapped != boundary_mapping.end()) {
            rebound_boundaries.emplace_back(mapped->second, observation);
            continue;
        }
        boundary_mapping.emplace(index, recipient.graph.boundary_count() + inserted_boundaries.size());
        inserted_boundaries.push_back(observation);
    }

    const auto donor_prefix = scope_roots ? donor.semantic_scope(scope_roots->first) : std::nullopt;
    const auto recipient_prefix = scope_roots ? recipient.semantic_scope(scope_roots->second) : std::nullopt;
    std::map<std::optional<std::size_t>, TemplateSemanticPath> semantic_paths;
    for (const auto index : donor_nodes) {
        auto node = donor.graph.at(index);
        node.begin = boundary_mapping.at(node.begin);
        node.end = boundary_mapping.at(node.end);
        inserted_nodes.push_back({ node, &donor });
        // An unscoped CPU fragment still has the donor's proven serving owner.
        // Rebase it only when that owner belongs to this qualified cut.
        const auto observed_scope = donor.observation(node).source_scope;
        const auto scope = node.kind == SourceSpanKind::CpuControl && !observed_scope ? std::optional(donor.source_call) : observed_scope;
        const auto [path, added] = semantic_paths.try_emplace(scope);
        if (added && donor_prefix && recipient_prefix && donor.semantic_scope(scope, scope_roots->first)) {
            const auto original = donor.semantic_scope(scope);
            if (original)
                path->second = rebase_semantic_path(std::make_shared<const std::vector<std::string_view>>(original->path),
                                                    donor_prefix->path.size(),
                                                    recipient_prefix->path);
        }
        inserted_nodes.back().semantic_path = path->second;

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
    for (const auto & edge : rotary_dependencies_) result[definition.graph[edge.producer].end].push_back(definition.graph[edge.consumer].begin);
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
      incoming_request_ready_(definition.incoming_request_ready),
      source_boundary_count_(definition.graph.boundary_count()),
      source_lane_count_(definition.lanes.size()) {
    for (std::size_t index = 0; index < definition.graph.size(); ++index) nodes_.push_back(index);
    for (std::size_t index = 0; index < definition.graph.dependencies().size(); ++index) dependencies_.push_back(index);

    struct Cut {
        std::size_t interface;
        std::size_t ready;
        std::size_t done;
    };
    std::map<std::size_t, Cut> transport;
    std::map<std::size_t, Cut> seams;
    std::set<std::size_t> substitute_nodes;
    for (std::size_t index = 0; index < definition.collective_interfaces.size(); ++index) {
        const auto & source = definition.collective_interfaces[index];
        const auto owner = source.owner_region;
        substitute_nodes.insert(source.substitute_nodes.begin(), source.substitute_nodes.end());
        const auto ready = source_boundary_count_ + inserted_boundaries_.size();
        inserted_boundaries_.push_back({ { source.lane }, definition.boundary_observations[source.host.begin].source_boundary, &definition.source_manifest });
        const auto done = source_boundary_count_ + inserted_boundaries_.size();
        inserted_boundaries_.push_back({ { source.lane }, definition.boundary_observations[source.host.end].source_boundary, &definition.source_manifest });
        collective_interfaces_.push_back({ &definition, index, source.host, ready, done, source.model_layer, nullptr, owner });
        added_dependencies_.push_back({ SourceRelation::ModelDataDependency, ready, done, owner });
        if (source.host_message) {
            // Transport may progress during native enqueue. Submission return
            // remains an observed CPU point, never a new launch barrier.
            for (const auto point : source.inputs) added_dependencies_.push_back({ SourceRelation::CollectiveSubmission, point, ready, owner });
            for (const auto point : source.outputs) added_dependencies_.push_back({ SourceRelation::NativeHostWait, done, point, owner });
            continue;
        }
        for (const auto point : source.transport_boundaries) {
            transport.emplace(point, Cut{ index, ready, done });
            boundary_aliases_[point] = { ready, done };
        }
        for (const auto edge : source.seam_dependencies) seams.emplace(edge, Cut{ index, ready, done });
        // Empty PG1 transport has no device task to carry the native CPU flow.
        // The producer qualified this exact launch and task-order slot.
        if (source.transport_boundaries.empty())
            for (const auto point : source.launch_inputs) added_dependencies_.push_back({ SourceRelation::CollectiveSubmission, point, ready, owner });
    }
    std::erase_if(dependencies_, [&](std::size_t index) {
        const auto & edge = definition.graph.dependencies()[index];
        const auto from = transport.find(edge.source), to = transport.find(edge.target);
        const auto seam = seams.find(index);
        if (from == transport.end() && to == transport.end() && seam == seams.end()) return false;
        if (from != transport.end() && to != transport.end() && from->second.interface == to->second.interface) return true;
        const auto owner = seam != seams.end() ? seam->second.interface : to != transport.end() ? to->second.interface : from->second.interface;
        const auto region = definition.collective_interfaces[owner].owner_region;
        if (seam != seams.end()) {
            added_dependencies_.emplace_back(TemplateDependency{ edge.relation, edge.source, seam->second.ready },
                                             region,
                                             TemplateDependencyOrigin{ TemplateDependencyKind::Local, index });
            added_dependencies_.emplace_back(TemplateDependency{ edge.relation, seam->second.done, edge.target },
                                             region,
                                             TemplateDependencyOrigin{ TemplateDependencyKind::Local, index });
        }
        else
            added_dependencies_.emplace_back(TemplateDependency{ edge.relation,
                                                                 from == transport.end() ? edge.source : from->second.done,
                                                                 to == transport.end() ? edge.target : to->second.ready },
                                             region,
                                             TemplateDependencyOrigin{ TemplateDependencyKind::Local, index });
        return true;
    });
    // Removed observer device services keep their original zero-time causal
    // paths. Their CPU launches and surrounding stream commands are untouched.
    std::erase_if(nodes_, [&](auto index) { return substitute_nodes.contains(index); });
}

std::expected<void, std::string> TemplateTopology::apply_operations(const TemplateDefinition & definition, std::span<const TemplateOperationRewrite> removals) {
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
    struct ResourceRegion {
        TemplateRegionPort port;
        std::optional<std::size_t> region;
        std::size_t first_node;
        std::size_t last_node;
    };
    std::map<TemplateResource, std::vector<ResourceRegion>> resource_regions;
    for (const auto & port : replacement.added_resource_ports_) resource_regions[port.resource].push_back({ port, port.recipient_region, 0, 0 });
    for (const auto & removal : removals) {
        // Plans use their own insertion offsets. Relocate each plan into the
        // instance before combining it with other independent replacements.
        const auto offset = replacement.inserted_boundaries_.size();
        const auto lane_offset = replacement.inserted_lanes_.size();
        const auto owner = removal.source_region ? removal.source_region : definition.region_owner(removal.nodes);
        for (const auto & [point, source] : removal.rebound_boundaries) {
            auto observation = source;
            if (observation.boundary.lane >= source_lane_count_) observation.boundary.lane += lane_offset;
            const auto target = point < source_boundary_count_ ? point : point + offset;
            const auto [previous, added] = replacement.rebound_boundaries_.try_emplace(target, observation);
            if (!added && (previous->second.boundary != observation.boundary || !previous->second.same_cpu_boundary(observation)))
                return std::unexpected("operation_cpu_interface_observation_conflict");
        }
        for (auto edge : removal.added_dependencies) {
            if (edge.source >= source_boundary_count_) edge.source += offset;
            if (edge.target >= source_boundary_count_) edge.target += offset;
            replacement.added_dependencies_.emplace_back(edge, owner);
        }
        const auto first_node = replacement.inserted_nodes_.size();
        for (auto inserted : removal.inserted_nodes) {
            inserted.recipient_region = owner;
            auto & node = inserted.node;
            if (node.begin >= source_boundary_count_) node.begin += offset;
            if (node.end >= source_boundary_count_) node.end += offset;
            if (inserted.stream_wait) inserted.stream_wait->completion = node.end;
            replacement.inserted_nodes_.push_back(std::move(inserted));
        }
        for (auto boundary : removal.inserted_boundaries) {
            if (boundary.boundary.lane >= source_lane_count_) boundary.boundary.lane += lane_offset;
            replacement.inserted_boundaries_.push_back(boundary);
        }
        replacement.inserted_lanes_.insert(replacement.inserted_lanes_.end(), removal.inserted_lanes.begin(), removal.inserted_lanes.end());
        for (auto port : removal.added_resource_ports) {
            if (port.entry >= source_boundary_count_) port.entry += offset;
            if (port.exit >= source_boundary_count_) port.exit += offset;
            if (port.device_done && *port.device_done >= source_boundary_count_) *port.device_done += offset;
            replacement.added_resource_ports_.emplace_back(port, owner);
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

    for (const auto & rewrite : removals) {
        if (!rewrite.sequence) continue;
        const auto applied = replacement.apply_sequence(definition, *rewrite.sequence);
        if (!applied) return std::unexpected(applied.error());
    }
    *this = std::move(replacement);
    return {};
}

void TemplateTopology::bind_inserted_kernel(std::size_t index, SourceKernel kernel) { inserted_nodes_.at(index).kernel_override = std::move(kernel); }

void TemplateTopology::bind_inserted_transfer(std::size_t index, SourceTransfer transfer) { inserted_nodes_.at(index).transfer_override = transfer; }

void TemplateTopology::bind_collective_work(std::size_t index, const CollectiveWork & work) { collective_interfaces_.at(index).work = &work; }

std::size_t TemplateTopology::boundary_alias(std::size_t boundary, bool incoming) const {
    for (auto found = boundary_aliases_.find(boundary); found != boundary_aliases_.end(); found = boundary_aliases_.find(boundary))
        boundary = incoming ? found->second.first : found->second.second;
    return boundary;
}

TemplateInsertedBoundary TemplateTopology::boundary_observation(const TemplateDefinition & definition, std::size_t boundary) const {
    const auto rebound = rebound_boundaries_.find(boundary);
    if (rebound != rebound_boundaries_.end()) return rebound->second;
    if (boundary >= source_boundary_count_) return inserted_boundaries_.at(boundary - source_boundary_count_);
    return { definition.graph.boundary(boundary),
             definition.boundary_observations.at(boundary).source_boundary,
             &definition.source_manifest,
             definition.cpu_boundary(boundary) };
}

} // namespace markov
