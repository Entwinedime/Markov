#include "markov/templates/templates.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace markov {

TemplateNode TemplateGraph::resolve(std::size_t index, const Location & location) const {
    if (location.region == 0) return local_[location.member];

    const auto & binding = bindings_[location.region - 1];
    auto node = binding.structure->nodes[location.member];
    node.begin = binding.boundaries[node.begin];
    node.end = binding.boundaries[node.end];
    node.observation_slot = index;
    return node;
}

TemplateNode TemplateGraph::operator[](std::size_t index) const { return locations_.empty() ? local_[index] : resolve(index, locations_[index]); }

TemplateNode TemplateGraph::at(std::size_t index) const { return locations_.empty() ? local_.at(index) : resolve(index, locations_.at(index)); }

TemplateDependency TemplateGraph::resolve_dependency(const Location & location) const {
    if (location.region == 0) return local_dependencies_[location.member];

    const auto & binding = bindings_[location.region - 1];
    if (location.member < binding.structure->dependencies.size()) {
        auto edge = binding.structure->dependencies[location.member];
        edge.source = binding.boundaries[edge.source];
        edge.target = binding.boundaries[edge.target];
        return edge;
    }

    const auto & port = binding.structure->dependency_ports[location.member - binding.structure->dependencies.size()];
    const auto local = binding.boundaries[port.connection.local_boundary];
    const auto remote = *binding.remote_boundaries[port.remote_boundary].local_boundary;
    const bool incoming = port.connection.direction == TemplateConnectionDirection::Incoming;
    return { port.connection.relation, incoming ? remote : local, incoming ? local : remote };
}

TemplateDependency TemplateGraph::dependency(std::size_t index) const {
    return dependency_locations_.empty() ? local_dependencies_[index] : resolve_dependency(dependency_locations_[index]);
}

TemplateDependency TemplateGraph::dependency_at(std::size_t index) const {
    return dependency_locations_.empty() ? local_dependencies_.at(index) : resolve_dependency(dependency_locations_.at(index));
}

TemplateBoundary TemplateGraph::boundary(std::size_t index) const {
    if (boundary_locations_.empty()) return local_boundaries_[index];

    const auto & location = boundary_locations_[index];
    if (location.region == 0) return local_boundaries_[location.member];

    const auto & binding = bindings_[location.region - 1];
    const auto point = binding.structure->boundaries[location.member];
    return { *binding.lanes[point.lane] };
}

TemplateConnectionEndpoint TemplateGraph::connection(std::size_t index) const {
    if (connection_locations_.empty() || connection_locations_[index].region == 0) {
        const auto & observed = local_connections_[connection_locations_.empty() ? index : connection_locations_[index].member];
        return { observed.contract, observed.remote_source_boundary };
    }

    const auto & location = connection_locations_[index];
    const auto & binding = bindings_[location.region - 1];
    const auto & port = binding.structure->connections[location.member];
    auto contract = port.connection;
    contract.local_boundary = binding.boundaries[contract.local_boundary];
    return { contract, binding.remote_boundaries[port.remote_boundary].source_boundary };
}

const ModelParameterRule & TemplateGraph::model_rule(std::size_t node) const {
    if (locations_.empty() || locations_[node].region == 0) return *model_rules_.at(node);

    const auto & location = locations_[node];
    return *bindings_[location.region - 1].structure->model_rules.at(location.member);
}

const CollectiveParameterRule & TemplateGraph::collective_rule(std::size_t node) const {
    if (locations_.empty() || locations_[node].region == 0) return *collective_rules_.at(node);

    const auto & location = locations_[node];
    return *bindings_[location.region - 1].structure->collective_rules.at(location.member);
}

void TemplateGraph::share(const TemplateDefinition & source, std::span<const std::size_t> regions, TemplateRegionStructures & pool) {
    if (regions.empty()) return;

    std::vector<std::size_t> node_owners(local_.size());
    std::map<std::size_t, std::optional<std::size_t>> boundary_owners;
    for (std::size_t region = 0; region < regions.size(); ++region) {
        for (const auto index : source.regions[regions[region]].nodes) {
            if (node_owners[index] != 0) throw std::invalid_argument("shared model regions must not overlap");
            node_owners[index] = region + 1;
            const auto & node = local_[index];
            for (const auto point : { node.begin, node.end }) {
                const auto [position, inserted] = boundary_owners.try_emplace(point, region + 1);
                if (!inserted && position->second != region + 1) position->second.reset();
            }
        }
    }

    // A physical task join is internal only when all contributors belong to
    // the same region. Serving completion exports and cross-rank edges stay out.
    std::map<std::size_t, std::optional<std::size_t>> joins;
    for (const auto & edge : local_dependencies_) {
        if (edge.relation != SourceRelation::CompletionJoin
            || source.lanes[source.graph.boundary(edge.target).lane].source.domain != SourceDomain::DeviceTaskCompletion)
            continue;
        const auto found = boundary_owners.find(edge.source);
        const auto owner = found == boundary_owners.end() ? std::nullopt : found->second;
        const auto [position, inserted] = joins.try_emplace(edge.target, owner);
        if (!inserted && position->second != owner) position->second.reset();
    }
    for (const auto & observed : local_connections_) {
        const auto & connection = observed.contract;
        if (connection.direction != TemplateConnectionDirection::Incoming) continue;
        const auto found = joins.find(connection.local_boundary);
        if (found != joins.end()) found->second.reset();
    }
    for (const auto & [point, owner] : joins) {
        if (!owner) continue;
        const auto [position, inserted] = boundary_owners.try_emplace(point, owner);
        if (!inserted && position->second != owner) position->second.reset();
    }

    // Endpoint aliasing does not transfer an outside span's extent to a region.
    std::set<std::pair<std::size_t, std::size_t>> retained_extents;
    for (std::size_t index = 0; index < local_.size(); ++index) {
        if (node_owners[index] == 0) retained_extents.emplace(local_[index].begin, local_[index].end);
    }
    std::vector<std::vector<std::size_t>> region_edges(regions.size());
    for (std::size_t index = 0; index < local_dependencies_.size(); ++index) {
        const auto & edge = local_dependencies_[index];
        if (edge.relation == SourceRelation::IntervalExtent && retained_extents.contains({ edge.source, edge.target })) continue;
        const auto from = boundary_owners.find(edge.source);
        const auto to = boundary_owners.find(edge.target);
        if (from == boundary_owners.end() || to == boundary_owners.end() || !from->second || from->second != to->second) continue;
        region_edges[*from->second - 1].push_back(index);
    }

    std::vector<std::vector<std::pair<std::size_t, TemplateConnectionDirection>>> region_ports(regions.size());
    for (std::size_t index = 0; index < local_dependencies_.size(); ++index) {
        const auto & edge = local_dependencies_[index];
        const auto from = boundary_owners.find(edge.source);
        const auto to = boundary_owners.find(edge.target);
        const auto source_owner = from == boundary_owners.end() ? std::nullopt : from->second;
        const auto target_owner = to == boundary_owners.end() ? std::nullopt : to->second;
        // Same-region retained extents still belong to outside work. Leave the
        // original dependency local instead of changing its ownership.
        if (source_owner == target_owner) continue;
        if (source_owner) region_ports[*source_owner - 1].emplace_back(index, TemplateConnectionDirection::Outgoing);
        if (target_owner) region_ports[*target_owner - 1].emplace_back(index, TemplateConnectionDirection::Incoming);
    }

    std::vector<std::vector<std::size_t>> region_connections(regions.size());
    for (std::size_t index = 0; index < local_connections_.size(); ++index) {
        const auto owner = boundary_owners.find(local_connections_[index].contract.local_boundary);
        if (owner != boundary_owners.end() && owner->second) region_connections[*owner->second - 1].push_back(index);
    }
    connection_locations_.resize(local_connections_.size());

    std::map<BoundaryId, std::size_t> source_points;
    for (std::size_t index = 0; index < source.boundary_observations.size(); ++index)
        source_points.emplace(source.boundary_observations[index].source_boundary, index);

    locations_.resize(local_.size());
    dependency_locations_.resize(local_dependencies_.size());
    for (std::size_t region = 0; region < regions.size(); ++region) {
        auto members = source.regions[regions[region]].nodes;
        // Source time orders records within a lane, never independent resources.
        std::ranges::sort(members, [&](std::size_t left, std::size_t right) {
            const auto key = [&](std::size_t index) {
                const auto & node = local_[index];
                const auto & lane = source.lanes[source.graph.boundary(node.begin).lane].source;
                return std::tuple(lane.domain,
                                  lane.native_id,
                                  source.boundary_observations[node.begin].observed_time_ns,
                                  source.boundary_observations[node.end].observed_time_ns,
                                  index);
            };
            return key(left) < key(right);
        });

        Binding binding;
        std::map<std::size_t, std::size_t> points;
        TemplateRegionStructure structure;
        structure.nodes.reserve(members.size());
        for (const auto index : members) {
            auto node = local_[index];
            for (const auto point : { node.begin, node.end }) {
                const auto [position, inserted] = points.try_emplace(point, binding.boundaries.size());
                if (inserted) binding.boundaries.push_back(point);
            }
            node.begin = points.at(node.begin);
            node.end = points.at(node.end);
            node.observation_slot = structure.nodes.size();
            const auto rule = model_rules_.find(index);
            if (rule != model_rules_.end()) {
                structure.model_rules.emplace(node.observation_slot, rule->second);
                model_rules_.erase(rule);
            }
            const auto collective = collective_rules_.find(index);
            if (collective != collective_rules_.end()) {
                structure.collective_rules.emplace(node.observation_slot, collective->second);
                collective_rules_.erase(collective);
            }
            locations_[index] = { bindings_.size() + 1, structure.nodes.size() };
            structure.nodes.push_back(node);
        }

        std::vector<std::pair<TemplateDependency, std::size_t>> edges;
        for (const auto index : region_edges[region]) {
            auto edge = local_dependencies_[index];
            for (const auto point : { edge.source, edge.target }) {
                const auto [position, inserted] = points.try_emplace(point, binding.boundaries.size());
                if (inserted) binding.boundaries.push_back(point);
            }
            edge.source = points.at(edge.source);
            edge.target = points.at(edge.target);
            edges.emplace_back(edge, index);
        }
        std::ranges::sort(edges, [](const auto & left, const auto & right) { return left.first < right.first; });
        for (const auto & [edge, index] : edges) {
            dependency_locations_[index] = { bindings_.size() + 1, structure.dependencies.size() };
            structure.dependencies.push_back(edge);
        }

        // Both forms use one remote endpoint table, so aliases across a local
        // dependency and a cross-definition connection are preserved as well.
        std::vector<std::tuple<TemplateConnectionContract, bool, std::size_t>> ports;
        for (const auto index : region_connections[region]) {
            auto contract = local_connections_[index].contract;
            contract.local_boundary = points.at(contract.local_boundary);
            ports.emplace_back(contract, true, index);
        }
        for (const auto & [index, direction] : region_ports[region]) {
            const auto & edge = local_dependencies_[index];
            const auto local = direction == TemplateConnectionDirection::Incoming ? edge.target : edge.source;
            ports.emplace_back(TemplateConnectionContract{ edge.relation, direction, points.at(local) }, false, index);
        }
        std::ranges::sort(ports);

        std::map<BoundaryId, std::size_t> remote_points;
        std::vector<SourceLane> remote_lanes;
        for (const auto & [contract, external, index] : ports) {
            BoundaryId remote;
            SourceLane lane;
            if (external) {
                remote = local_connections_[index].remote_source_boundary;
                lane = local_connections_[index].remote_lane;
            }
            else {
                const auto & edge = local_dependencies_[index];
                const auto point = contract.direction == TemplateConnectionDirection::Incoming ? edge.source : edge.target;
                remote = source.boundary_observations[point].source_boundary;
                lane = source.lanes[local_boundaries_[point].lane].source;
            }
            const auto [point, inserted] = remote_points.try_emplace(remote, binding.remote_boundaries.size());
            if (inserted) {
                const auto local = source_points.find(remote);
                binding.remote_boundaries.push_back({ remote, local == source_points.end() ? std::nullopt : std::optional(local->second) });
                remote_lanes.push_back(lane);
            }

            if (external) {
                connection_locations_[index] = { bindings_.size() + 1, structure.connections.size() };
                structure.connections.push_back({ contract, point->second });
            }
            else {
                // The two cuts reference one source edge slot, never two edges.
                if (dependency_locations_[index].region == 0)
                    dependency_locations_[index] = { bindings_.size() + 1, structure.dependencies.size() + structure.dependency_ports.size() };
                structure.dependency_ports.push_back({ contract, point->second });
            }
        }

        // Native resources establish aliasing. Shared slots retain domain and
        // relative rank only; zero always denotes this serving definition's rank.
        std::map<TemplateResource, std::size_t> resources;
        std::map<std::uint32_t, std::size_t> ranks{
            { source.rank, 0 }
        };
        for (const auto point : binding.boundaries) {
            const auto lane_index = local_boundaries_[point].lane;
            const auto & lane = source.lanes[lane_index].source;
            const auto [resource, inserted] = resources.try_emplace(TemplateResource{ lane.domain, lane.rank, lane.native_id }, structure.resources.size());
            if (inserted) {
                binding.lanes.push_back(lane_index);
                const auto [rank, added] = ranks.try_emplace(lane.rank, ranks.size());
                structure.resources.push_back({ lane.domain, rank->second });
            }
            structure.boundaries.push_back({ resource->second });
        }

        for (std::size_t index = 0; index < binding.remote_boundaries.size(); ++index) {
            const auto & remote = binding.remote_boundaries[index];
            const auto & lane = remote_lanes[index];
            const auto [resource, inserted] = resources.try_emplace(TemplateResource{ lane.domain, lane.rank, lane.native_id }, structure.resources.size());
            if (inserted) {
                binding.lanes.push_back(remote.local_boundary ? std::optional(local_boundaries_[*remote.local_boundary].lane) : std::nullopt);
                const auto [rank, added] = ranks.try_emplace(lane.rank, ranks.size());
                structure.resources.push_back({ lane.domain, rank->second });
            }
            std::optional<std::size_t> local_alias;
            if (remote.local_boundary) {
                const auto local = points.find(*remote.local_boundary);
                if (local != points.end()) local_alias = local->second;
            }
            structure.remote_boundaries.push_back({ resource->second, local_alias });
        }

        const auto [position, inserted] = pool.insert(std::move(structure));
        binding.structure = &*position;
        bindings_.push_back(std::move(binding));
    }

    boundary_locations_.resize(local_boundaries_.size());
    for (std::size_t region = 0; region < bindings_.size(); ++region) {
        const auto & binding = bindings_[region];
        for (std::size_t member = 0; member < binding.boundaries.size(); ++member) {
            const auto point = binding.boundaries[member];
            // Ambiguous endpoints stay source-local; no region acquires ownership.
            if (boundary_owners.at(point) == region + 1) boundary_locations_[point] = { region + 1, member };
        }
    }
    std::vector<TemplateBoundary> remaining_boundaries;
    for (std::size_t index = 0; index < local_boundaries_.size(); ++index) {
        if (boundary_locations_[index].region != 0) continue;
        boundary_locations_[index].member = remaining_boundaries.size();
        remaining_boundaries.push_back(local_boundaries_[index]);
    }
    local_boundaries_ = std::move(remaining_boundaries);

    // Only unshared records remain local. Original indices and all external
    // relations still resolve to their original source-local endpoints.
    std::vector<TemplateNode> remaining_nodes;
    for (std::size_t index = 0; index < local_.size(); ++index) {
        if (locations_[index].region != 0) continue;
        locations_[index].member = remaining_nodes.size();
        remaining_nodes.push_back(local_[index]);
    }
    local_ = std::move(remaining_nodes);

    std::vector<TemplateDependency> remaining_edges;
    for (std::size_t index = 0; index < local_dependencies_.size(); ++index) {
        if (dependency_locations_[index].region != 0) continue;
        dependency_locations_[index].member = remaining_edges.size();
        remaining_edges.push_back(local_dependencies_[index]);
    }
    local_dependencies_ = std::move(remaining_edges);

    std::vector<ObservedConnection> remaining_connections;
    for (std::size_t index = 0; index < local_connections_.size(); ++index) {
        if (connection_locations_[index].region != 0) continue;
        connection_locations_[index].member = remaining_connections.size();
        remaining_connections.push_back(local_connections_[index]);
    }
    local_connections_ = std::move(remaining_connections);
}

} // namespace markov
