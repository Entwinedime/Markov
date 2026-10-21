#include "markov/templates/templates.hpp"

#include "markov/graph/cpu_service_partition.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace markov {
namespace {

TemplateDependency source_dependency(const TemplateDefinition & source, TemplateDependencyOrigin origin) {
    if (origin.kind == TemplateDependencyKind::Local) return source.graph.dependencies()[origin.slot];
    const auto & edge = source.rotary_position_dependencies[origin.slot];
    return { SourceRelation::ModelDataDependency, source.graph[edge.producer].end, source.graph[edge.consumer].begin };
}

bool same_parameter_binding(const auto & bindings, SourceSpanId left, const auto & peer_bindings, SourceSpanId right) {
    const auto a = bindings.find(left), b = peer_bindings.find(right);
    if (a == bindings.end() || b == peer_bindings.end()) return a == bindings.end() && b == peer_bindings.end();
    return a->second == b->second;
}

struct SequenceInput {
    std::size_t edge;
    std::optional<TemplateResource> previous_resource;
    std::optional<std::size_t> producer_step;
};

struct SequenceSlice {
    SequenceSlice(const TemplateRegionSequence & source, std::size_t slot) : sequence(&source), step(slot) {}

    const TemplateRegionSequence * sequence;
    std::size_t step;
    std::vector<std::size_t> nodes;
    std::set<std::size_t> boundaries;
    std::vector<TemplateRegionPort> ports;
    std::set<std::size_t> internal_edges, interface_edges, retained_edges;
    std::vector<std::size_t> collectives;
    std::vector<TemplateOwnedResourcePort> resources;
    std::vector<SequenceInput> inputs;
    std::set<std::size_t> exports;
};

// A value view of the current graph, after its child transformations. It never
// owns source observations and does not reintroduce an already removed child.
class SequenceGraph {
public:
    struct Node {
        TemplateInsertedNode work;
        std::size_t slot;
        bool inserted;
    };

    SequenceGraph(const TemplateDefinition & source, const TemplateTopology & topology) : source(source), incoming_ready(topology.incoming_request_ready()) {
        lanes = source.lanes;
        lanes.insert(lanes.end(), topology.inserted_lanes().begin(), topology.inserted_lanes().end());
        for (std::size_t point = 0; point < source.graph.boundary_count() + topology.inserted_boundaries().size(); ++point)
            boundaries.push_back(topology.boundary_observation(source, point));

        std::map<std::optional<std::size_t>, TemplateSemanticPath> semantic_paths;
        for (const auto slot : topology.nodes()) {
            TemplateInsertedNode node{ source.graph[slot], &source };
            const auto observed_scope = node.observation().source_scope;
            const auto call = node.node.kind == SourceSpanKind::CpuControl && !observed_scope ? std::optional(source.source_call) : observed_scope;
            const auto scope = source.semantic_scope(call);
            if (scope) node.input_slot = scope->input_slot;
            const auto [path, added] = semantic_paths.try_emplace(call);
            if (added && scope) path->second = std::make_shared<const std::vector<std::string_view>>(scope->path);
            node.semantic_path = path->second;
            if (node.node.kind == SourceSpanKind::HostWait) {
                const auto wait = std::ranges::find(source.stream_waits, node.observation().source_span, &TemplateStreamWait::source_span);
                if (wait != source.stream_waits.end()) node.stream_wait = *wait;
            }
            nodes.push_back({ std::move(node), slot, false });
        }
        for (std::size_t slot = 0; slot < topology.inserted_nodes().size(); ++slot) nodes.push_back({ topology.inserted_nodes()[slot], slot, true });
        collectives = topology.collective_interfaces();
        resources = topology.added_resource_ports();
        for (const auto slot : topology.dependencies())
            edges.emplace_back(source.graph.dependencies()[slot], std::nullopt, TemplateDependencyOrigin{ TemplateDependencyKind::Local, slot });
        edges.insert(edges.end(), topology.added_dependencies().begin(), topology.added_dependencies().end());
        for (std::size_t slot = 0; slot < topology.rotary_dependencies().size(); ++slot) {
            const auto & edge = topology.rotary_dependencies()[slot];
            const std::size_t members[]{ edge.producer, edge.consumer };
            edges.emplace_back(TemplateDependency{ SourceRelation::ModelDataDependency, source.graph[edge.producer].end, source.graph[edge.consumer].begin },
                               source.region_owner(members),
                               TemplateDependencyOrigin{ TemplateDependencyKind::ModelData, slot });
        }

        for (std::size_t index = 0; index < edges.size(); ++index) {
            const auto & edge = edges[index];
            paths.connect(edge.source, edge.target);
            if (edge.origin) source_edges.emplace(*edge.origin, index);
        }
        for (const auto & node : nodes) paths.connect(node.work.node.begin, node.work.node.end);
        reverse = paths.reversed();
        for (std::size_t point = 0; point < boundaries.size(); ++point) {
            entries.push_back(topology.boundary_alias(point, true));
            exits.push_back(topology.boundary_alias(point));
        }
    }

    std::expected<SequenceSlice, std::string> slice(const TemplateRegionSequence & sequence, std::size_t step) const {
        const auto & contract = source.sequence_step(sequence, step);
        const auto owner = step % 2 == 0 ? std::optional(sequence.source_members[step / 2]) : std::nullopt;
        if (!contract.resource_ports) return std::unexpected(contract.resource_ports.error());
        SequenceSlice result(sequence, step);
        result.ports = *contract.resource_ports;
        for (auto & port : result.ports) {
            port.entry = entries[port.entry];
            port.exit = exits[port.exit];
            if (port.device_done) port.device_done = exits[*port.device_done];
        }
        for (const auto point : contract.boundaries) {
            result.boundaries.insert(entries[point]);
            result.boundaries.insert(exits[point]);
        }
        for (std::size_t slot = 0; slot < nodes.size(); ++slot) {
            const auto & node = nodes[slot];
            const bool owned = node.inserted ? owner && owns(*owner, node.work.recipient_region) : std::ranges::binary_search(contract.nodes, node.slot);
            if (!owned) continue;
            result.nodes.push_back(slot);
            result.boundaries.insert(node.work.node.begin);
            result.boundaries.insert(node.work.node.end);
        }
        for (std::size_t slot = 0; slot < collectives.size(); ++slot) {
            const auto & port = collectives[slot];
            if (owner) {
                if (!owns(*owner, port.recipient_region)) continue;
            }
            else {
                const auto & original = port.definition->collective_interfaces[port.interface];
                if (port.definition != &source || !std::ranges::includes(contract.nodes, source.regions[original.owner_region].nodes)) continue;
            }
            result.collectives.push_back(slot);
            for (const auto point : { port.host.begin, port.host.end, port.ready, port.done }) result.boundaries.insert(point);
        }
        for (const auto & port : resources) {
            if (!owner || !owns(*owner, port.recipient_region)) continue;
            result.resources.push_back(port);
            result.ports.push_back(port);
            for (const auto point : { port.entry, port.exit, port.device_done.value_or(port.exit) }) result.boundaries.insert(point);
        }
        // Child rewrites may extend a declared resource interface. Only those
        // ports participate in projection; causality never selects extra work.
        std::map<TemplateResource, std::vector<TemplateRegionPort>> ports;
        for (const auto & port : result.ports) ports[port.resource].push_back(port);
        result.ports.clear();
        for (const auto & [resource, candidates] : ports) {
            std::set<std::size_t> first, last;
            for (const auto & port : candidates) {
                first.insert(port.entry);
                last.insert(port.device_done.value_or(port.exit));
            }
            bool extended = candidates.size() != 1;
            for (const auto slot : result.collectives) {
                const auto & collective = collectives[slot];
                if (lanes[boundaries[collective.ready].boundary.lane].resource() != resource) continue;
                extended = true;
                first.insert(collective.ready);
                last.insert(collective.done);
            }
            if (!extended) {
                result.ports.push_back(candidates.front());
                continue;
            }
            first = paths.frontier(std::move(first));
            last = reverse.frontier(std::move(last));
            if (first.size() != 1 || last.size() != 1) return std::unexpected("sequence_resource_frontier_not_unique");
            result.ports.push_back({ resource, *first.begin(), *last.begin() });
            if (resource.domain == SourceDomain::DeviceStream) result.ports.back().device_done = *last.begin();
        }
        for (const auto & input : sequence.inputs) {
            const auto active = source_edges.find(input.dependency);
            if (active == source_edges.end()) continue; // A removed child contributes no input or export.
            const auto & edge = edges[active->second];
            if (input.step == step) result.inputs.push_back({ active->second, input.previous_resource, input.producer_step });
            if (input.producer_step == step) {
                if (!result.boundaries.contains(edge.source)) return std::unexpected("region_sequence_export_not_owned");
                result.exports.insert(edge.source);
            }
        }
        partition_edges(result, &contract, owner);
        return result;
    }

    bool owns(std::size_t region, std::optional<std::size_t> owner) const { return owner && source.contains_region(region, *owner); }

    void partition_edges(SequenceSlice & slice, const TemplateSubgraphRegion * contract, std::optional<std::size_t> owner) const {
        std::set<std::pair<std::size_t, std::size_t>> outside_extents;
        for (std::size_t index = 0; index < nodes.size(); ++index)
            if (!std::ranges::binary_search(slice.nodes, index)) outside_extents.emplace(nodes[index].work.node.begin, nodes[index].work.node.end);
        for (std::size_t index = 0; index < edges.size(); ++index) {
            const auto & edge = edges[index];
            const bool from = slice.boundaries.contains(edge.source), to = slice.boundaries.contains(edge.target);
            const bool retained = edge.relation == SourceRelation::IntervalExtent && outside_extents.contains({ edge.source, edge.target });
            if (retained) {
                if (from || to) slice.retained_edges.insert(index);
                continue;
            }
            bool internal = false;
            if (contract && edge.origin && edge.origin->kind == TemplateDependencyKind::Local)
                internal = std::ranges::binary_search(contract->internal_dependencies, edge.origin->slot);
            else internal = !owner || owns(*owner, edge.recipient_region);
            if (from && to && internal) slice.internal_edges.insert(index);
            else if (from || to) slice.interface_edges.insert(index);
        }
    }

    const TemplateDefinition & source;
    std::vector<std::pair<std::size_t, std::size_t>> incoming_ready;
    std::vector<std::size_t> entries, exits;
    std::vector<TemplateLane> lanes;
    std::vector<TemplateInsertedBoundary> boundaries;
    std::vector<Node> nodes;
    std::vector<TemplateCollectiveInstance> collectives;
    std::vector<TemplateOwnedResourcePort> resources;
    std::vector<TemplateOwnedDependency> edges;
    std::map<TemplateDependencyOrigin, std::size_t> source_edges;
    DependencyPaths<std::size_t> paths, reverse;
};

// Repetition qualifies both the normalized own work and its local interfaces.
// Shared storage alone never grants interchangeability to a loop seam.
class SeamSignature {
public:
    SeamSignature(const TemplateDefinition & source, const TemplateRegionSequence & sequence, std::size_t index)
        : source_(source),
          cut_(sequence.separators[index]) {
        if (!cut_.nodes.empty()) {
            body_ = source.graph.structure(cut_.nodes.front());
            valid_ = body_ && body_->nodes.size() == cut_.nodes.size();
            if (body_) {
                next_ = body_->boundaries.size();
                for (const auto point : cut_.boundaries)
                    if (const auto local = source.graph.local_boundary(cut_.nodes.front(), point)) points_.emplace(point, *local);
            }
        }
        for (const auto & port : *cut_.resource_ports) {
            for (const auto point : { port.entry, port.exit, port.device_done.value_or(port.exit) })
                if (!points_.contains(point)) points_.emplace(point, next_++);
            resources_.emplace_back(port.resource,
                                    *coordinate(port.entry),
                                    *coordinate(port.exit),
                                    port.device_done ? coordinate(*port.device_done) : std::nullopt);
        }
        for (const auto index : cut_.incoming_dependencies) {
            const auto edge = source.graph.dependencies()[index];
            interfaces_.emplace_back(edge.relation,
                                     TemplateConnectionDirection::Incoming,
                                     coordinate(edge.target),
                                     std::nullopt,
                                     source.lanes[source.graph.boundary(edge.source).lane].resource());
        }
        for (const auto index : cut_.outgoing_dependencies) {
            const auto edge = source.graph.dependencies()[index];
            interfaces_.emplace_back(edge.relation,
                                     TemplateConnectionDirection::Outgoing,
                                     coordinate(edge.source),
                                     std::nullopt,
                                     source.lanes[source.graph.boundary(edge.target).lane].resource());
        }
        for (const auto index : cut_.retained_dependencies) {
            const auto edge = source.graph.dependencies()[index];
            interfaces_.emplace_back(edge.relation, TemplateConnectionDirection::Incoming, coordinate(edge.source), coordinate(edge.target), std::nullopt);
        }
        for (const auto index : cut_.external_connections) {
            const auto connection = source.connection(index);
            interfaces_.emplace_back(connection.relation, connection.direction, coordinate(connection.local_boundary), std::nullopt, connection.resource);
        }
        for (const auto & input : sequence.inputs) {
            if (input.step != index * 2 + 1) continue;
            const auto edge = source_dependency(source, input.dependency);
            inputs_.emplace_back(edge.relation,
                                 coordinate(edge.target),
                                 input.previous_resource,
                                 input.producer_step,
                                 input.previous_resource ? std::nullopt : std::optional(edge.source));
        }
        std::ranges::sort(interfaces_);
        std::ranges::sort(inputs_);
        bind_work();
    }

    bool matches(const SeamSignature & other) const {
        if (!valid_ || !other.valid_ || body_ != other.body_ || resources_ != other.resources_ || interfaces_ != other.interfaces_ || inputs_ != other.inputs_
            || cpu_services_ != other.cpu_services_ || waits_ != other.waits_ || nodes_.size() != other.nodes_.size())
            return false;

        auto peer = other.nodes_.begin();
        for (const auto & [key, node] : nodes_) {
            if (key != peer->first || !same_work(node, other.source_, peer->second)) return false;
            ++peer;
        }
        return true;
    }

private:
    void bind_work() {
        CpuServicePartition cpu(source_.graph.boundary_count());
        for (const auto & [point, name] : source_.cpu_boundaries) cpu.name(point, &name);
        for (const auto & edge : source_.graph.dependencies()) cpu.connect(edge.source, edge.target, edge.relation);

        std::size_t cpu_fragments = 0;
        for (const auto index : cut_.nodes) {
            const auto node = source_.graph[index];
            if (source_.graph.structure(index) != body_) valid_ = false;
            const auto begin = coordinate(node.begin), end = coordinate(node.end);
            if (!begin || !end) {
                valid_ = false;
                continue;
            }
            if (!nodes_.emplace(std::tuple(node.kind, *begin, *end), index).second) valid_ = false;
            const auto & observed = source_.observation(node);
            if (node.kind == SourceSpanKind::CpuControl && !observed.transfer) {
                cpu.add({ index, node.begin, node.end, 0, source_.rank });
                ++cpu_fragments;
            }
            else if (node.kind == SourceSpanKind::HostWait) {
                cpu.wait(node.begin, node.end);
                const auto wait = std::ranges::find(source_.stream_waits, observed.source_span, &TemplateStreamWait::source_span);
                if (wait == source_.stream_waits.end()) valid_ = false;
                else waits_.emplace_back(*begin, *end, wait->scope, wait->resources);
            }
            else if (node.role() != TemplateNodeRole::Annotation) {
                cpu.protect(node.begin);
                cpu.protect(node.end);
            }
        }

        for (const auto & port : *cut_.resource_ports) {
            cpu.protect(port.entry);
            cpu.protect(port.exit);
        }
        for (const auto & [port, point] : source_.ports) cpu.protect(point);
        for (const auto & [slot, point] : source_.incoming_request_ready) cpu.protect(point);
        for (const auto & collective : source_.collective_interfaces) {
            cpu.protect(collective.host.begin);
            cpu.protect(collective.host.end);
        }

        std::size_t named_fragments = 0;
        for (auto & unit : cpu.units()) {
            named_fragments += unit.members.size();
            const auto begin = coordinate(source_.graph[unit.members.front()].begin), end = coordinate(unit.end);
            if (!begin || !end) valid_ = false;
            else cpu_services_.emplace_back(*begin, *end, std::move(unit.service));
        }
        if (named_fragments != cpu_fragments) valid_ = false;
        std::ranges::sort(cpu_services_);
        std::ranges::sort(waits_);
    }

    bool same_work(std::size_t node, const TemplateDefinition & peer_source, std::size_t peer_node) const {
        const auto value = source_.graph[node];
        const auto & work = source_.observation(value);
        const auto & peer = peer_source.observation(peer_source.graph[peer_node]);
        // Shared topology deliberately omits observation payloads. Repetition
        // must preserve actual work as well as the independently bound rules.
        if (bool(work.kernel) != bool(peer.kernel) || bool(work.transfer) != bool(peer.transfer) || work.device_command != peer.device_command) return false;
        if (work.kernel && *work.kernel != *peer.kernel) return false;
        if (work.transfer && std::tie(work.transfer->bytes, work.transfer->direction) != std::tie(peer.transfer->bytes, peer.transfer->direction)) return false;
        if ((value.kind == SourceSpanKind::Compute && !work.kernel) || (value.kind == SourceSpanKind::Dma && !work.transfer)
            || (value.kind == SourceSpanKind::DeviceMarker && !work.device_command))
            return false;
        if (!same_parameter_binding(source_.transfer_layouts, work.source_span, peer_source.transfer_layouts, peer.source_span)
            || !same_parameter_binding(source_.kv_kernel_layouts, work.source_span, peer_source.kv_kernel_layouts, peer.source_span)
            || !same_parameter_binding(source_.batch_kernel_layouts, work.source_span, peer_source.batch_kernel_layouts, peer.source_span)
            || !same_parameter_binding(source_.request_pool_layouts, work.source_span, peer_source.request_pool_layouts, peer.source_span))
            return false;
        const auto attention = source_.attention_kernel_layouts.find(work.source_span);
        const auto peer_attention = peer_source.attention_kernel_layouts.find(peer.source_span);
        if ((attention == source_.attention_kernel_layouts.end()) != (peer_attention == peer_source.attention_kernel_layouts.end())) return false;
        if (attention != source_.attention_kernel_layouts.end() && *attention->second != *peer_attention->second) return false;
        if (value.kind == SourceSpanKind::ResourceObservation) {
            const auto use = std::ranges::find(source_.workspace_uses, work.source_span, &TemplateWorkspaceUse::marker);
            const auto other = std::ranges::find(peer_source.workspace_uses, peer.source_span, &TemplateWorkspaceUse::marker);
            if ((use == source_.workspace_uses.end()) != (other == peer_source.workspace_uses.end())) return false;
            if (use != source_.workspace_uses.end()
                && (use->pool != other->pool || use->branch != other->branch || use->requested_bytes != other->requested_bytes
                    || use->requires_device_synchronization != other->requires_device_synchronization
                    || (use->branch != WorkspaceBranch::Release && (!use->demand || !other->demand || **use->demand != **other->demand))))
                return false;
        }
        return true;
    }

    std::optional<std::size_t> coordinate(std::size_t point) {
        const auto found = points_.find(point);
        if (found != points_.end()) return found->second;
        if (std::ranges::binary_search(cut_.boundaries, point)) valid_ = false;
        return std::nullopt;
    }

    const TemplateDefinition & source_;
    const TemplateSubgraphRegion & cut_;
    const TemplateRegionStructure * body_ = nullptr;
    std::map<std::size_t, std::size_t> points_;
    std::size_t next_ = 0;
    bool valid_ = true;
    std::vector<std::tuple<TemplateResource, std::size_t, std::size_t, std::optional<std::size_t>>> resources_;
    std::vector<
        std::tuple<SourceRelation, TemplateConnectionDirection, std::optional<std::size_t>, std::optional<std::size_t>, std::optional<TemplateResource>>>
        interfaces_;
    std::vector<std::tuple<SourceRelation, std::optional<std::size_t>, std::optional<TemplateResource>, std::optional<std::size_t>, std::optional<std::size_t>>>
        inputs_;
    std::map<std::tuple<SourceSpanKind, std::size_t, std::size_t>, std::size_t> nodes_;
    std::vector<std::tuple<std::size_t, std::size_t, CpuServiceDescriptor>> cpu_services_;
    std::vector<std::tuple<std::size_t, std::size_t, SynchronizationScope, std::vector<TemplateResource>>> waits_;
};

} // namespace

TemplateSubgraphRegion TemplateDefinition::sequence_separator(const TemplateRegionSequence & sequence, std::size_t previous, std::size_t next) const {
    const auto resource = lanes[graph.boundary(entry).lane].resource();
    const auto & first = *std::ranges::find(*regions[previous].resource_ports, resource, &TemplateRegionPort::resource);
    const auto & last = *std::ranges::find(*regions[next].resource_ports, resource, &TemplateRegionPort::resource);
    DependencyPaths<std::size_t> control;
    for (const auto & edge : graph.dependencies()) {
        if (edge.relation != SourceRelation::ThreadOrder && edge.relation != SourceRelation::IntervalExtent) continue;
        if (lanes[graph.boundary(edge.source).lane].resource() == resource && lanes[graph.boundary(edge.target).lane].resource() == resource)
            control.connect(edge.source, edge.target);
    }
    const auto reached = control.reachable({ first.exit });
    const auto ancestors = control.reversed().reachable({ last.entry });
    const auto parent = regions[previous].parent_call;
    const auto enclosing = std::ranges::find(regions, parent, &TemplateRegion::source_call);
    std::set<std::size_t> members;
    for (std::size_t index = 0; index < graph.size(); ++index) {
        const auto node = graph[index];
        if (lanes[graph.boundary(node.begin).lane].resource() != resource || !reached.contains(node.begin) || !ancestors.contains(node.end)) continue;
        if (std::ranges::any_of(sequence.source_members, [&](auto member) { return std::ranges::binary_search(regions[member].nodes, index); })) continue;
        if (enclosing != regions.end()) {
            const auto owner = region_owner(std::span(&index, 1));
            if (!owner || !contains_region(static_cast<std::size_t>(enclosing - regions.begin()), *owner)) continue;
        }
        members.insert(index);
    }
    // Native operation associations, rather than device timestamps, bring any
    // asynchronous work submitted by the parent loop into the same seam.
    for (const auto & operation : operations) {
        if (lanes[graph.boundary(operation.host.begin).lane].resource() != resource || !reached.contains(operation.host.begin)
            || !ancestors.contains(operation.host.end))
            continue;
        const auto cut = operation_region(operation);
        for (const auto node : cut.nodes) {
            if (std::ranges::any_of(sequence.source_members, [&](auto member) { return std::ranges::binary_search(regions[member].nodes, node); })) continue;
            members.insert(node);
        }
    }
    TemplateSubgraphRegion result;
    result.nodes.assign(members.begin(), members.end());
    std::set<std::size_t> points{ first.exit, last.entry };
    for (const auto index : result.nodes) {
        points.insert(graph[index].begin);
        points.insert(graph[index].end);
    }
    result.boundaries.assign(points.begin(), points.end());
    return complete_region(std::move(result), TemplateRegionScope{ first.exit, last.entry });
}

void TemplateDefinition::bind_sequence(TemplateRegionSequence & sequence) const {
    const auto parent = regions[sequence.source_members.front()].parent_call;
    const auto host = lanes[graph.boundary(entry).lane].resource();
    for (const auto index : sequence.source_members) {
        const auto & region = regions[index];
        if (region.parent_call != parent) {
            sequence.gap = "region_sequence_parent_mismatch";
            return;
        }
        if (!region.resource_ports) {
            sequence.gap = region.resource_ports.error();
            return;
        }
        if (std::ranges::find(*region.resource_ports, host, &TemplateRegionPort::resource) == region.resource_ports->end()) {
            sequence.gap = "region_sequence_control_port_missing";
            return;
        }
    }
    for (std::size_t member = 1; member < sequence.source_members.size(); ++member) {
        auto separator = sequence_separator(sequence, sequence.source_members[member - 1], sequence.source_members[member]);
        if (!separator.resource_ports) {
            sequence.gap = separator.resource_ports.error();
            return;
        }
        sequence.separators.push_back(std::move(separator));
    }
    const auto steps = sequence.source_members.size() * 2 - 1;
    for (std::size_t step = 0; step < steps; ++step) {
        const auto & cut = sequence_step(sequence, step);
        std::vector<TemplateDependencyOrigin> inputs;
        for (const auto index : cut.incoming_dependencies) inputs.push_back({ TemplateDependencyKind::Local, index });
        for (const auto index : cut.incoming_rotary_dependencies) inputs.push_back({ TemplateDependencyKind::ModelData, index });
        for (const auto origin : inputs) {
            const auto edge = source_dependency(*this, origin);
            if (edge.relation == SourceRelation::ThreadOrder || edge.relation == SourceRelation::StreamOrder || edge.relation == SourceRelation::IntervalExtent)
                continue;
            TemplateSequenceInput input{ step, origin, std::nullopt, std::nullopt };
            for (std::size_t producer = 0; producer < steps; ++producer) {
                const auto & source = sequence_step(sequence, producer);
                const auto port = std::ranges::find_if(*source.resource_ports,
                                                       [&](const auto & port) { return port.exit == edge.source || port.device_done == edge.source; });
                const bool owns = std::ranges::any_of(source.nodes, [&](auto node) { return graph[node].end == edge.source; });
                if (!owns && port == source.resource_ports->end()) continue;
                if (producer >= step) {
                    sequence.gap = "sequence_carried_data_interface_not_qualified";
                    return;
                }
                if (port != source.resource_ports->end()) input.previous_resource = port->resource;
                else input.producer_step = producer;
                break;
            }
            sequence.inputs.push_back(std::move(input));
        }
    }
}

void TemplateDefinition::qualify_sequences() {
    for (auto & sequence : sequences) {
        if (sequence.gap || sequence.separators.empty()) continue;
        const auto repeated = sequence.separators.size() - 1;
        const SeamSignature prototype(*this, sequence, repeated);
        bool same = true;
        for (std::size_t index = 0; index < sequence.separators.size(); ++index) same = same && prototype.matches(SeamSignature(*this, sequence, index));
        if (same) sequence.repeated_separator = repeated;
    }
}

std::expected<TemplateOperationRewrite, std::string> TemplateDefinition::plan_region_sequence(std::size_t index,
                                                                                              std::span<const TemplateRegionReference> members) const {
    const auto & sequence = sequences[index];
    if (sequence.gap) return std::unexpected(*sequence.gap);
    if (members.empty()) return std::unexpected("region_sequence_empty");
    if (members.size() > 1 && sequence.separators.empty()) return std::unexpected("region_sequence_separator_missing");
    if (members.size() > sequence.source_members.size() && !sequence.repeated_separator)
        return std::unexpected("region_sequence_repeated_separator_not_qualified");
    for (const auto & member : members) {
        const auto & donor = *member.definition;
        const bool input_prefix = input_reception_sequence == index && donor.input_reception_sequence == member.sequence;
        if (donor.rank != rank || donor.step != step || (donor.phase != phase && !input_prefix)) return std::unexpected("region_sequence_donor_mismatch");
        if (const auto & pattern = donor.sequences[member.sequence]; pattern.gap) return std::unexpected(*pattern.gap);
        if (source_manifest != donor.source_manifest && (!has_resource_roles() || !donor.has_resource_roles()))
            return std::unexpected("cross_source_resource_role_not_qualified");
    }
    TemplateOperationRewrite result;
    result.sequence = TemplateSequenceRewrite{
        &sequence,
        { members.begin(), members.end() }
    };
    return result;
}

// Expansion owns the temporary binding state. The enclosing topology rewrite
// stages this whole operation before any target instance is committed.
class TemplateTopology::SequenceExpansion {
public:
    SequenceExpansion(TemplateTopology & target, const TemplateDefinition & definition, const TemplateSequenceRewrite & sequence)
        : target_(target),
          definition_(definition),
          sequence_(sequence),
          recipient_(definition, target) {}

    void bind_donor(const TemplateDefinition & definition, const TemplateTopology & topology) { donors_.try_emplace(&definition, definition, topology); }

    std::expected<void, std::string> apply() {
        const auto prepared = prepare();
        if (!prepared) return prepared;

        for (std::size_t slot = 0; slot < sequence_.members.size(); ++slot) {
            const auto & reference = sequence_.members[slot];
            const auto & graph = donor(*reference.definition);
            const auto key = std::tuple(reference.definition, reference.sequence, reference.member);
            auto found = slices_.find(key);
            if (found == slices_.end()) {
                auto slice = graph.slice(reference.definition->sequences[reference.sequence], reference.member * 2);
                if (!slice) return std::unexpected(slice.error());
                found = slices_.emplace(key, std::move(*slice)).first;
            }
            const auto copied = append(graph, found->second, slot, false);
            if (!copied) return copied;
            if (slot + 1 < sequence_.members.size()) {
                const auto seam = slot < separators_.size() ? slot : *sequence_.source->repeated_separator;
                const auto joined = append(recipient_, separators_[seam], slot, true);
                if (!joined) return joined;
            }
        }
        for (const auto & [resource, point] : exits_) {
            const auto relation = resource.domain == SourceDomain::CpuThread ? SourceRelation::ThreadOrder : SourceRelation::StreamOrder;
            edges_.push_back({ relation, frontiers_.at(resource), point });
        }
        commit();
        return {};
    }

private:
    std::expected<void, std::string> prepare() {
        const auto & source = *sequence_.source;
        const auto parent = definition_.regions[source.source_members.front()].parent_call;
        if (parent != definition_.source_call)
            sequence_owner_ =
                static_cast<std::size_t>(std::ranges::find(definition_.regions, parent, &TemplateRegion::source_call) - definition_.regions.begin());
        std::vector<SequenceSlice> originals;
        for (std::size_t step = 0; step < source.source_members.size() * 2 - 1; ++step) {
            auto slice = recipient_.slice(source, step);
            if (!slice) return std::unexpected(slice.error());
            for (const auto & port : slice->ports) {
                entries_.try_emplace(port.resource, port.entry);
                exits_[port.resource] = port.exit;
            }
            if (step % 2 == 0) {
                member_points_.insert(slice->boundaries.begin(), slice->boundaries.end());
                originals.push_back(std::move(*slice));
            }
            else separators_.push_back(std::move(*slice));
        }
        for (const auto & slice : originals) {
            const auto removed = remove(slice);
            if (!removed) return removed;
        }
        for (const auto & slice : separators_) {
            const auto removed = remove(slice);
            if (!removed) return removed;
        }
        frontiers_ = entries_;
        return {};
    }

    std::expected<void, std::string> remove(const SequenceSlice & slice) {
        removed_points_.insert(slice.boundaries.begin(), slice.boundaries.end());
        removed_edges_.insert(slice.internal_edges.begin(), slice.internal_edges.end());
        interface_edges_.insert(slice.interface_edges.begin(), slice.interface_edges.end());
        retained_edges_.insert(slice.retained_edges.begin(), slice.retained_edges.end());
        removed_collectives_.insert(slice.collectives.begin(), slice.collectives.end());
        for (const auto node : slice.nodes)
            if (!removed_nodes_.insert(node).second) return std::unexpected("region_sequence_members_overlap");
        return {};
    }

    const SequenceGraph & donor(const TemplateDefinition & definition) {
        if (&definition == &definition_) return recipient_;
        auto found = donors_.find(&definition);
        if (found == donors_.end()) found = donors_.try_emplace(&definition, definition, TemplateTopology(definition)).first;
        return found->second;
    }

    std::size_t bind_lane(const TemplateLane & lane) {
        const auto original = std::ranges::find_if(definition_.lanes, [&](const auto & candidate) { return candidate.resource() == lane.resource(); });
        if (original != definition_.lanes.end()) return static_cast<std::size_t>(original - definition_.lanes.begin());
        const auto inserted = std::ranges::find_if(target_.inserted_lanes_, [&](const auto & candidate) { return candidate.resource() == lane.resource(); });
        if (inserted != target_.inserted_lanes_.end()) return definition_.lanes.size() + static_cast<std::size_t>(inserted - target_.inserted_lanes_.begin());
        const auto index = definition_.lanes.size() + target_.inserted_lanes_.size();
        target_.inserted_lanes_.push_back(lane);
        return index;
    }

    std::expected<void, std::string> append(const SequenceGraph & graph, const SequenceSlice & slice, std::size_t slot, bool separator) {
        const auto & source_members = sequence_.source->source_members;
        const auto owner = separator ? sequence_owner_ : std::optional(source_members[std::min(slot, source_members.size() - 1)]);
        const auto source_root = separator ? graph.source.regions[slice.sequence->source_members.front()].parent_call
                                           : graph.source.regions[slice.sequence->source_members[slice.step / 2]].source_call;
        const auto source_owner = std::ranges::find(graph.source.regions, source_root, &TemplateRegion::source_call);
        const auto source_prefix = graph.source.semantic_scope(source_root);
        const auto target_prefix = definition_.semantic_scope(owner ? definition_.regions[*owner].source_call : definition_.source_call);
        std::map<const std::vector<std::string_view> *, TemplateSemanticPath> semantic_paths;
        const auto previous = frontiers_;
        const auto first_edge = edges_.size();
        std::map<std::size_t, std::size_t> mapping;
        for (const auto point : slice.boundaries) {
            auto boundary = graph.boundaries[point];
            boundary.boundary.lane = bind_lane(graph.lanes[boundary.boundary.lane]);
            const auto destination = target_.source_boundary_count_ + target_.inserted_boundaries_.size();
            mapping.emplace(point, destination);
            target_.inserted_boundaries_.push_back(boundary);
            if (&graph == &recipient_ && (!separator || !member_points_.contains(point))) {
                const auto [entry, fresh] = aliases_.try_emplace(point, destination, destination);
                if (!fresh) entry->second.second = destination;
            }
        }
        // These are declared data values, not a last-seen boundary cache. One
        // source export must denote exactly one target producer invocation.
        for (const auto point : slice.exports) {
            const auto key = std::tuple(slice.sequence, slice.step, point);
            if (!exports_.emplace(key, mapping.at(point)).second) return std::unexpected("region_sequence_export_producer_not_unique");
        }
        for (const auto & port : slice.ports) {
            const auto found = frontiers_.find(port.resource);
            if (found == frontiers_.end()) return std::unexpected("region_sequence_resource_not_bound");
            const auto relation = port.resource.domain == SourceDomain::CpuThread ? SourceRelation::ThreadOrder : SourceRelation::StreamOrder;
            edges_.push_back({ relation, found->second, mapping.at(port.entry) });
            found->second = mapping.at(port.exit);
        }
        std::set<std::size_t> copied_edges = slice.internal_edges;
        copied_edges.insert(slice.interface_edges.begin(), slice.interface_edges.end());
        for (const auto index : copied_edges) {
            const auto & edge = graph.edges[index];
            if (!slice.boundaries.contains(edge.target)) continue;
            if (slice.boundaries.contains(edge.source)) {
                if (!slice.internal_edges.contains(index)) continue;
                edges_.push_back({ edge.relation, mapping.at(edge.source), mapping.at(edge.target) });
                continue;
            }
            // A span outside this member can share its boundary. Its extent
            // stays with the recipient; it is not an input value of the donor.
            if (edge.relation == SourceRelation::IntervalExtent || edge.relation == SourceRelation::ThreadOrder || edge.relation == SourceRelation::StreamOrder)
                continue;
            const auto input = std::ranges::find(slice.inputs, index, &SequenceInput::edge);
            if (input != slice.inputs.end() && input->previous_resource)
                edges_.push_back({ edge.relation, previous.at(*input->previous_resource), mapping.at(edge.target) });
            else if (input != slice.inputs.end() && input->producer_step) {
                const auto producer = exports_.find({ slice.sequence, *input->producer_step, edge.source });
                if (producer == exports_.end()) return std::unexpected("region_sequence_export_not_bound");
                edges_.push_back({ edge.relation, producer->second, mapping.at(edge.target) });
            }
            else if (&graph == &recipient_ && !removed_points_.contains(edge.source)) edges_.push_back({ edge.relation, edge.source, mapping.at(edge.target) });
            else return std::unexpected("region_sequence_input_not_bound");
        }
        for (const auto index : slice.nodes) {
            auto node = graph.nodes[index].work;
            // The slice already qualified work membership. Keep child rewrites'
            // complete paths and rebind the enclosing cut, rather than reading
            // their donor paths again when an outer member is repeated.
            const auto observed_scope = node.observation().source_scope;
            const auto call = node.node.kind == SourceSpanKind::CpuControl && !observed_scope ? std::optional(graph.source.source_call) : observed_scope;
            const bool scoped = graph.nodes[index].inserted
                                    ? source_root == graph.source.source_call
                                          || (source_owner != graph.source.regions.end()
                                              && graph.owns(static_cast<std::size_t>(source_owner - graph.source.regions.begin()), node.recipient_region))
                                    : graph.source.semantic_scope(call, source_root).has_value();
            if (scoped && source_prefix && target_prefix && node.semantic_path) {
                const auto [path, added] = semantic_paths.try_emplace(node.semantic_path.get());
                if (added) path->second = rebase_semantic_path(node.semantic_path, source_prefix->path.size(), target_prefix->path);
                node.semantic_path = path->second;
            }
            else node.semantic_path.reset();
            node.node.begin = mapping.at(node.node.begin);
            node.node.end = mapping.at(node.node.end);
            if (node.stream_wait) node.stream_wait->completion = node.node.end;
            if (!separator) {
                if (sequence_.source->parameter == TemplateSequenceParameter::Request) node.request_slot = slot;
                else if (sequence_.source->parameter == TemplateSequenceParameter::IncomingRequest) node.input_slot = slot;
                else if (sequence_.source->parameter == TemplateSequenceParameter::ModelLayer) node.model_layer = static_cast<std::uint32_t>(slot);
            }
            node.recipient_region = owner;
            nodes_.push_back(std::move(node));
        }
        if (!separator && sequence_.source->parameter == TemplateSequenceParameter::IncomingRequest) {
            const auto & member = graph.source.regions[slice.sequence->source_members[slice.step / 2]];
            if (!member.cpu_scope) return std::unexpected("input_request_return_not_observed");
            incoming_ready_.emplace_back(slot, mapping.at(graph.exits[member.cpu_scope->end]));
        }
        if (sequence_.source->parameter == TemplateSequenceParameter::InputReception) {
            for (const auto & [slot, point] : graph.incoming_ready) {
                const auto bound = mapping.find(graph.exits[point]);
                if (bound != mapping.end()) incoming_ready_.emplace_back(slot, bound->second);
            }
        }
        for (const auto index : slice.collectives) {
            auto port = graph.collectives[index];
            if (!mapping.contains(port.ready) || !mapping.contains(port.done)) return std::unexpected("sequence_collective_interface_not_closed");
            port.host = { mapping.at(port.host.begin), mapping.at(port.host.end) };
            port.ready = mapping.at(port.ready);
            port.done = mapping.at(port.done);
            if (!separator && sequence_.source->parameter == TemplateSequenceParameter::ModelLayer) port.model_layer = static_cast<std::uint32_t>(slot);
            port.recipient_region = owner;
            collectives_.push_back(port);
        }
        for (auto port : slice.resources) {
            port.entry = mapping.at(port.entry);
            port.exit = mapping.at(port.exit);
            if (port.device_done) port.device_done = mapping.at(*port.device_done);
            if (!separator) port.recipient_region = owner;
            resources_.push_back(std::move(port));
        }
        for (auto index = first_edge; index < edges_.size(); ++index) edges_[index].recipient_region = owner;
        return {};
    }

    void commit() {
        // Observed cross-template identities rebind to the first/last target
        // use. Original outer resource gates remain stable across replacement.
        const auto resource_gate = [&](auto point) {
            return std::ranges::any_of(entries_, [point](const auto & entry) { return entry.second == point; })
                   || std::ranges::any_of(exits_, [point](const auto & entry) { return entry.second == point; });
        };
        for (const auto & [point, alias] : aliases_) {
            if (!resource_gate(point)) target_.boundary_aliases_[point] = alias;
        }
        std::vector<TemplateOwnedDependency> retained;
        for (std::size_t index = 0; index < recipient_.edges.size(); ++index) {
            auto edge = recipient_.edges[index];
            const bool from = removed_points_.contains(edge.source), to = removed_points_.contains(edge.target);
            if (retained_edges_.contains(index)) {
                retained.push_back(edge);
                continue;
            }
            if (removed_edges_.contains(index) || (interface_edges_.contains(index) && from && to)) continue;
            // External FIFO and service extents meet at the retained gate.
            // Bypassing it would fork the serial resource around its copy.
            if (from && !resource_gate(edge.source) && aliases_.contains(edge.source)) edge.source = aliases_.at(edge.source).second;
            if (to && !resource_gate(edge.target) && aliases_.contains(edge.target)) edge.target = aliases_.at(edge.target).first;
            retained.push_back(edge);
        }
        retained.insert(retained.end(), edges_.begin(), edges_.end());
        target_.dependencies_.clear();
        target_.rotary_dependencies_.clear();
        target_.added_dependencies_ = std::move(retained);
        std::set<std::size_t> removed_original, removed_inserted;
        for (const auto index : removed_nodes_) {
            const auto & node = recipient_.nodes[index];
            (node.inserted ? removed_inserted : removed_original).insert(node.slot);
        }
        std::erase_if(target_.nodes_, [&](auto slot) { return removed_original.contains(slot); });
        std::vector<TemplateInsertedNode> retained_nodes;
        for (std::size_t slot = 0; slot < target_.inserted_nodes_.size(); ++slot)
            if (!removed_inserted.contains(slot)) retained_nodes.push_back(std::move(target_.inserted_nodes_[slot]));
        retained_nodes.insert(retained_nodes.end(), std::make_move_iterator(nodes_.begin()), std::make_move_iterator(nodes_.end()));
        target_.inserted_nodes_ = std::move(retained_nodes);
        std::vector<TemplateCollectiveInstance> retained_collectives;
        for (std::size_t index = 0; index < target_.collective_interfaces_.size(); ++index)
            if (!removed_collectives_.contains(index)) retained_collectives.push_back(target_.collective_interfaces_[index]);
        target_.collective_interfaces_ = std::move(retained_collectives);
        target_.collective_interfaces_.insert(target_.collective_interfaces_.end(), collectives_.begin(), collectives_.end());
        std::erase_if(target_.added_resource_ports_, [&](const auto & port) {
            return std::ranges::any_of(sequence_.source->source_members, [&](auto region) { return recipient_.owns(region, port.recipient_region); });
        });
        target_.added_resource_ports_.insert(target_.added_resource_ports_.end(), resources_.begin(), resources_.end());
        if (sequence_.source->parameter == TemplateSequenceParameter::IncomingRequest
            || sequence_.source->parameter == TemplateSequenceParameter::InputReception)
            target_.incoming_request_ready_ = std::move(incoming_ready_);
    }

    TemplateTopology & target_;
    const TemplateDefinition & definition_;
    const TemplateSequenceRewrite & sequence_;
    SequenceGraph recipient_;
    std::vector<SequenceSlice> separators_;
    std::set<std::size_t> removed_points_, removed_nodes_, member_points_;
    std::set<std::size_t> removed_edges_, interface_edges_, retained_edges_, removed_collectives_;
    std::optional<std::size_t> sequence_owner_;
    std::map<TemplateResource, std::size_t> entries_, exits_, frontiers_;
    std::map<const TemplateDefinition *, SequenceGraph> donors_;
    std::map<std::tuple<const TemplateDefinition *, std::size_t, std::size_t>, SequenceSlice> slices_;
    std::map<std::tuple<const TemplateRegionSequence *, std::size_t, std::size_t>, std::size_t> exports_;
    std::map<std::size_t, std::pair<std::size_t, std::size_t>> aliases_;
    std::vector<TemplateOwnedDependency> edges_;
    std::vector<TemplateInsertedNode> nodes_;
    std::vector<TemplateCollectiveInstance> collectives_;
    std::vector<TemplateOwnedResourcePort> resources_;
    std::vector<std::pair<std::size_t, std::size_t>> incoming_ready_;
};

std::expected<void, std::string> TemplateTopology::apply_sequence(const TemplateDefinition & definition, const TemplateSequenceRewrite & sequence) {
    return SequenceExpansion(*this, definition, sequence).apply();
}

std::expected<void, std::string> TemplateTopology::replace_sequence(const TemplateDefinition & definition, const TemplateSequenceRewrite & sequence,
                                                                    const TemplateDefinition & donor, const TemplateTopology & prepared) {
    auto replacement = *this;
    SequenceExpansion expansion(replacement, definition, sequence);
    expansion.bind_donor(donor, prepared);
    const auto applied = expansion.apply();
    if (applied) *this = std::move(replacement);
    return applied;
}

} // namespace markov
