#include "markov/templates/templates.hpp"

#include <algorithm>
#include <compare>
#include <cstddef>
#include <map>
#include <numeric>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace markov {

namespace {

template <class Rule> std::strong_ordering compare_rules(const std::map<std::size_t, const Rule *> & left, const std::map<std::size_t, const Rule *> & right) {
    return std::lexicographical_compare_three_way(left.begin(), left.end(), right.begin(), right.end(), [](const auto & a, const auto & b) {
        if (const auto order = a.first <=> b.first; order != 0) return order;
        return *a.second <=> *b.second;
    });
}

// Hierarchy and physical ownership are resolved before graph canonicalization.
// Only these value arrays survive the source-call traversal.
class RegionPartition {
public:
    RegionPartition(const TemplateDefinition & source, std::span<const std::size_t> regions, std::size_t node_count) {
        const auto root = regions.size();
        const auto region_count = root + 1;
        std::map<std::size_t, const TemplateRegion *> observed_regions;
        for (const auto & region : source.regions) observed_regions.emplace(region.source_call, &region);

        std::map<std::size_t, std::size_t> selected;
        for (std::size_t index = 0; index < root; ++index) selected.emplace(source.regions[regions[index]].source_call, index);
        std::vector<std::size_t> parents(root, root);
        for (std::size_t index = 0; index < root; ++index) {
            auto parent = source.regions[regions[index]].parent_call;
            while (parent != source.source_call) {
                if (const auto found = selected.find(parent); found != selected.end()) {
                    parents[index] = found->second;
                    break;
                }
                parent = observed_regions.at(parent)->parent_call;
            }
        }

        std::vector<std::size_t> depths(region_count);
        for (std::size_t index = 0; index < root; ++index)
            for (auto current = index; current != root; current = parents[current]) ++depths[index];

        order.resize(region_count);
        std::iota(order.begin(), order.end(), 0);
        std::ranges::sort(order, [&](std::size_t left, std::size_t right) {
            const auto entered = [&](std::size_t index) {
                const auto point = index == root ? source.entry : source.regions[regions[index]].cpu_scope->begin;
                return source.boundary_observations[point].observed_time_ns;
            };
            return std::tuple(depths[left], entered(left), left) < std::tuple(depths[right], entered(right), right);
        });

        std::vector<std::size_t> node_owners(node_count, root + 1);
        // Shallow scopes yield to their selected descendants. A physical span has
        // one owner; parent composition never introduces another executable copy.
        for (const auto region : order) {
            if (region == root) continue;
            for (const auto index : source.regions[regions[region]].nodes) node_owners[index] = region + 1;
        }
        for (const auto region : regions) semantics.push_back(source.regions[region].semantic);
        semantics.push_back("serving." + std::string(ServingContract{}.block(source.step).name));
        for (const auto & sequence : source.sequences) {
            if (sequence.gap) continue;
            const auto parent = source.regions[sequence.source_members.front()].parent_call;
            const auto enclosing = std::ranges::find(source.regions, parent, &TemplateRegion::source_call);
            const auto semantic = enclosing == source.regions.end() ? semantics[root] : enclosing->semantic;
            for (const auto & seam : sequence.separators) {
                if (seam.nodes.empty()) continue;
                const auto owner = semantics.size();
                semantics.push_back(semantic + "/between");
                order.push_back(owner);
                // A repeated seam is part of the parent's composition. Its
                // physical work leaves the fixed parent body exactly once.
                for (const auto node : seam.nodes) node_owners[node] = owner + 1;
            }
        }
        members.resize(semantics.size());
        for (std::size_t index = 0; index < node_count; ++index) members[node_owners[index] - 1].push_back(index);
    }

    std::vector<std::vector<std::size_t>> members;
    std::vector<std::size_t> order;
    std::vector<std::string> semantics;
};

} // namespace

std::strong_ordering TemplateRegionStructure::operator<=>(const TemplateRegionStructure & other) const {
    const auto fields = std::tie(resources, boundaries, nodes, dependencies, semantic);
    const auto peer_fields = std::tie(other.resources, other.boundaries, other.nodes, other.dependencies, other.semantic);
    if (const auto order = fields <=> peer_fields; order != 0) return order;

    // Borrowed rule addresses establish ownership, never reusable structure identity.
    return compare_rules(model_rules, other.model_rules);
}

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
    auto edge = binding.structure->dependencies[location.member];
    edge.source = binding.boundaries[edge.source];
    edge.target = binding.boundaries[edge.target];
    return edge;
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
    return { binding.lanes[point.lane] };
}

TemplateConnectionEndpoint TemplateGraph::connection(std::size_t index) const { return connections_[index]; }

const ModelParameterRule * TemplateGraph::model_rule(std::size_t node) const {
    const bool shared = !locations_.empty() && locations_[node].region != 0;
    const auto & rules = shared ? bindings_[locations_[node].region - 1].structure->model_rules : model_rules_;
    const auto found = rules.find(shared ? locations_[node].member : node);
    return found == rules.end() ? nullptr : found->second;
}

const TemplateRegionStructure * TemplateGraph::structure(std::size_t node) const {
    const auto & location = locations_[node];
    return location.region == 0 ? nullptr : bindings_[location.region - 1].structure;
}

std::optional<std::size_t> TemplateGraph::local_boundary(std::size_t node, std::size_t boundary) const {
    const auto & location = locations_[node];
    if (location.region == 0) return std::nullopt;
    const auto & points = bindings_[location.region - 1].boundaries;
    const auto found = std::ranges::find(points, boundary);
    if (found == points.end()) return std::nullopt;
    return static_cast<std::size_t>(found - points.begin());
}

void TemplateGraph::share(const TemplateDefinition & source, std::span<const std::size_t> regions, TemplateRegionStructures & pool) {
    RegionPartition partition(source, regions, local_.size());
    const auto region_count = partition.members.size();
    auto & region_members = partition.members;
    const auto & order = partition.order;

    std::map<std::size_t, std::optional<std::size_t>> boundary_owners;
    for (std::size_t region = 0; region < region_count; ++region) {
        for (const auto index : region_members[region]) {
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
    for (const auto & observed : connections_) {
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

    std::vector<std::vector<std::size_t>> region_edges(region_count);
    // Share only same-owner internal edges. Cross-region edges keep their two
    // original endpoints once in this invocation, independent of peer count.
    for (std::size_t index = 0; index < local_dependencies_.size(); ++index) {
        const auto & edge = local_dependencies_[index];
        const auto from = boundary_owners.find(edge.source);
        const auto to = boundary_owners.find(edge.target);
        const auto source_owner = from == boundary_owners.end() ? std::nullopt : from->second;
        const auto target_owner = to == boundary_owners.end() ? std::nullopt : to->second;
        if (source_owner && source_owner == target_owner) region_edges[*source_owner - 1].push_back(index);
    }

    locations_.resize(local_.size());
    dependency_locations_.resize(local_dependencies_.size());
    // Child invocation count and selection belong to the existing region tree,
    // not the identity of the parent's own reusable work.
    for (const auto region : order) {
        auto members = std::move(region_members[region]);
        // Source time orders records within a lane, never independent resources.
        std::ranges::sort(members, [&](std::size_t left, std::size_t right) {
            const auto key = [&](std::size_t index) {
                const auto & node = local_[index];
                const auto & resource = source.lanes[source.graph.boundary(node.begin).lane];
                const auto & lane = resource.source;
                return std::tuple(lane.domain,
                                  resource.role,
                                  lane.rank == source.rank ? 0 : lane.rank + 1,
                                  resource.role ? 0 : lane.native_id,
                                  source.boundary_observations[node.begin].observed_time_ns,
                                  source.boundary_observations[node.end].observed_time_ns,
                                  index);
            };
            return key(left) < key(right);
        });

        Binding binding;
        std::map<std::size_t, std::size_t> points;
        TemplateRegionStructure structure;
        structure.semantic = partition.semantics[region];
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
                structure.resources.push_back({ lane.domain, rank->second, source.lanes[lane_index].role });
            }
            structure.boundaries.push_back({ resource->second });
        }

        const auto [position, inserted] = pool.insert(std::move(structure));
        binding.structure = &*position;
        bindings_.push_back(std::move(binding));
    }

    boundary_locations_.resize(local_boundaries_.size());
    for (std::size_t slot = 0; slot < bindings_.size(); ++slot) {
        const auto & binding = bindings_[slot];
        for (std::size_t member = 0; member < binding.boundaries.size(); ++member) {
            const auto point = binding.boundaries[member];
            // Ambiguous endpoints stay source-local; no region acquires ownership.
            if (boundary_owners.at(point) == order[slot] + 1) boundary_locations_[point] = { slot + 1, member };
        }
    }
    std::vector<TemplateBoundary> remaining_boundaries;
    for (std::size_t index = 0; index < local_boundaries_.size(); ++index) {
        if (boundary_locations_[index].region != 0) continue;
        boundary_locations_[index].member = remaining_boundaries.size();
        remaining_boundaries.push_back(local_boundaries_[index]);
    }
    local_boundaries_ = std::move(remaining_boundaries);

    // Every node has one region owner, including the serving root. Release the
    // producer storage; all original node indices now resolve through bindings.
    local_ = std::vector<TemplateNode>{};

    // Every remaining edge retains its original source-local endpoints, so
    // aliases between local dependencies and external connections stay intact.
    std::vector<TemplateDependency> remaining_edges;
    for (std::size_t index = 0; index < local_dependencies_.size(); ++index) {
        if (dependency_locations_[index].region != 0) continue;
        dependency_locations_[index].member = remaining_edges.size();
        remaining_edges.push_back(local_dependencies_[index]);
    }
    local_dependencies_ = std::move(remaining_edges);
}

} // namespace markov
