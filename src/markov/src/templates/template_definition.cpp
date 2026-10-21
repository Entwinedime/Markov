#include "markov/templates/templates.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace markov {
namespace {

using Json = nlohmann::json;

} // namespace

TemplateNodeRole TemplateNode::role() const {
    switch (kind) {
    case SourceSpanKind::CpuControl:
    case SourceSpanKind::Compute:
    case SourceSpanKind::Dma:
    case SourceSpanKind::DeviceMarker:
    case SourceSpanKind::HostCommunication:
        return TemplateNodeRole::Service;
    case SourceSpanKind::HostWait:
    case SourceSpanKind::DeviceWait:
        return TemplateNodeRole::Wait;
    case SourceSpanKind::CpuObservation:
    case SourceSpanKind::CommunicationObservation:
    case SourceSpanKind::ResourceObservation:
        return TemplateNodeRole::Annotation;
    }
    std::unreachable();
}

TemplateConnection TemplateDefinition::connection(std::size_t index) const {
    const auto endpoint = graph.connection(index);
    const auto & contract = endpoint.contract;
    const auto & observed = connection_observations[index];
    return { observed.source_edge,         contract.relation, contract.direction,   contract.local_boundary, endpoint.remote_source_boundary,
             observed.remote_source_calls, observed.resource, observed.remote_order };
}

std::optional<std::size_t> TemplateDefinition::operation_view_service(const TemplateOperation & operation) const {
    if (!operation.device_spans.empty() || !operation.workers.empty()) return std::nullopt;
    const auto region = operation_region(operation);
    if (!region.crossing_nodes.empty() || !region.external_connections.empty() || !region.incoming_rotary_dependencies.empty()
        || !region.outgoing_rotary_dependencies.empty())
        return std::nullopt;

    // Reuse the observed CPU service, not an empty alias node. Internal
    // annotation points can be omitted only when none exports a dependency.
    std::optional<std::size_t> service;
    for (const auto member : region.nodes) {
        const auto & node = graph[member];
        if (node.role() == TemplateNodeRole::Annotation) continue;
        if (service || node.kind != SourceSpanKind::CpuControl || node.begin != operation.host.begin || node.end != operation.host.end) return std::nullopt;
        service = member;
    }
    if (!service) return std::nullopt;

    bool enters = false;
    bool exits = false;
    for (const auto incoming : { true, false }) {
        const auto & cuts = incoming ? region.incoming_dependencies : region.outgoing_dependencies;
        const auto endpoint = incoming ? operation.host.begin : operation.host.end;
        for (const auto identity : cuts) {
            const auto & edge = graph.dependencies()[identity];
            if ((incoming ? edge.target : edge.source) != endpoint) return std::nullopt;
            if (edge.relation == SourceRelation::ThreadOrder) {
                if (incoming) enters = true;
                else exits = true;
            }
            else if (edge.relation != SourceRelation::IntervalExtent) { return std::nullopt; }
        }
    }
    if (!enters || !exits) return std::nullopt;
    return service;
}


TemplateSubgraphRegion TemplateDefinition::operation_region(const TemplateOperation & operation) const {
    std::vector<TemplateOperationWindow> windows{ operation.host };
    windows.insert(windows.end(), operation.workers.begin(), operation.workers.end());

    std::set<SourceSpanId> observed_members(operation.device_spans.begin(), operation.device_spans.end());
    return execution_region(windows, observed_members);
}

TemplateSubgraphRegion TemplateDefinition::workspace_region(const TemplateWorkspaceUse & use) const {
    // Allocation runs synchronously inside its worker; the enclosing attention
    // kernel is not owned by this window, even when both share the same scope.
    const TemplateOperationWindow window{ use.marker, use.begin, use.end };
    return execution_region(std::span(&window, 1), {});
}

TemplateSubgraphRegion TemplateDefinition::execution_region(std::span<const TemplateOperationWindow> windows,
                                                            const std::set<SourceSpanId> & observed_members) const {
    TemplateSubgraphRegion region;
    std::set<std::size_t> points;

    // Timestamps locate synchronous CPU windows on their own lanes only.
    // Device membership comes exclusively from the recorded asynchronous IDs.
    for (const auto & window : windows) {
        const auto & first = graph.boundary(window.begin);
        const auto first_ns = boundary_observations[window.begin].observed_time_ns;
        const auto last_ns = boundary_observations[window.end].observed_time_ns;
        for (std::size_t index = 0; index < graph.boundary_count(); ++index) {
            const auto & point = graph.boundary(index);
            const auto time_ns = boundary_observations[index].observed_time_ns;
            if (point.lane == first.lane && first_ns <= time_ns && time_ns <= last_ns) points.insert(index);
        }
    }

    for (std::size_t index = 0; index < graph.size(); ++index) {
        const auto & node = graph[index];
        if (observed_members.contains(observation(node).source_span)) {
            region.nodes.push_back(index);
            points.insert(node.begin);
            points.insert(node.end);
            continue;
        }
        const auto & begin = graph.boundary(node.begin);
        if (lanes[begin.lane].source.domain != SourceDomain::CpuThread) continue;

        const auto begin_ns = boundary_observations[node.begin].observed_time_ns;
        const auto end_ns = boundary_observations[node.end].observed_time_ns;

        for (const auto & window : windows) {
            const auto & first = graph.boundary(window.begin);
            const auto first_ns = boundary_observations[window.begin].observed_time_ns;
            const auto last_ns = boundary_observations[window.end].observed_time_ns;
            if (begin.lane != first.lane) continue;
            if (first_ns <= begin_ns && end_ns <= last_ns) {
                region.nodes.push_back(index);
                break;
            }
            // Enclosing CPU annotations are provenance, not partially owned work.
            if (node.role() != TemplateNodeRole::Annotation && begin_ns < last_ns && first_ns < end_ns) {
                region.crossing_nodes.push_back(index);
                break;
            }
        }
    }

    region.boundaries.assign(points.begin(), points.end());
    return complete_region(std::move(region));
}

std::size_t TemplateDefinition::declare_sequence(std::vector<std::size_t> members, TemplateSequenceParameter parameter, std::vector<std::size_t> expanded) {
    const auto index = sequences.size();
    sequences.push_back({ std::move(members), std::move(expanded), parameter, {}, {}, std::nullopt, std::nullopt });
    return index;
}

const TemplateSubgraphRegion & TemplateDefinition::sequence_step(const TemplateRegionSequence & sequence, std::size_t step) const {
    return step % 2 == 0 ? static_cast<const TemplateSubgraphRegion &>(regions[sequence.source_members[step / 2]]) : sequence.separators[step / 2];
}

void TemplateDefinition::finalize_regions() {
    std::set<std::size_t> replacements;
    for (const auto region : { transition_region, running_update_region })
        if (region) replacements.insert(*region);
    if (kv_region) replacements.insert(kv_region->region);
    for (const auto & [role, region] : filter_regions) replacements.insert(region);
    auto required = replacements;
    for (const auto & sequence : sequences) required.insert(sequence.source_members.begin(), sequence.source_members.end());

    // An added resource can only anchor to a sibling that covers all existing
    // outside work on that resource. Complete those candidates, not every scope.
    std::map<TemplateResource, std::vector<std::size_t>> resources;
    for (std::size_t index = 0; index < graph.size(); ++index) {
        const auto node = graph[index];
        if (node.role() != TemplateNodeRole::Annotation) resources[lanes[graph.boundary(node.begin).lane].resource()].push_back(index);
    }
    for (const auto recipient : replacements) {
        const auto & region = regions[recipient];
        for (const auto & [resource, members] : resources) {
            std::vector<std::size_t> outside;
            std::ranges::set_difference(members, region.nodes, std::back_inserter(outside));
            if (outside.empty()) continue;
            for (std::size_t candidate = 0; candidate < regions.size(); ++candidate) {
                const auto & sibling = regions[candidate];
                if (candidate == recipient || sibling.parent_call != region.parent_call || !std::ranges::includes(sibling.nodes, outside)) continue;
                if (std::ranges::any_of(sibling.nodes, [&](auto node) { return std::ranges::binary_search(region.nodes, node); })) continue;
                required.insert(candidate);
            }
        }
    }
    for (const auto index : required) {
        auto & region = regions[index];
        std::set<std::size_t> points(region.inputs.begin(), region.inputs.end());
        points.insert(region.outputs.begin(), region.outputs.end());
        if (region.cpu_scope) {
            points.insert(region.cpu_scope->begin);
            points.insert(region.cpu_scope->end);
        }
        for (const auto index : region.nodes) {
            points.insert(graph[index].begin);
            points.insert(graph[index].end);
        }
        region.boundaries.assign(points.begin(), points.end());
        auto & cut = static_cast<TemplateSubgraphRegion &>(region);
        cut = complete_region(std::move(cut), region.cpu_scope);
    }
    for (auto & sequence : sequences) bind_sequence(sequence);
}

bool TemplateDefinition::contains_region(std::size_t ancestor, std::size_t region) const {
    const auto call = regions[ancestor].source_call;
    for (auto current = regions[region].source_call; current != source_call;) {
        if (current == call) return true;
        current = std::ranges::find(regions, current, &TemplateRegion::source_call)->parent_call;
    }
    return false;
}

std::optional<std::size_t> TemplateDefinition::region_owner(std::span<const std::size_t> nodes) const {
    if (nodes.empty()) return std::nullopt;
    std::optional<std::size_t> owner;
    for (std::size_t index = 0; index < regions.size(); ++index) {
        const auto & region = regions[index];
        if (!std::ranges::includes(region.nodes, nodes)) continue;
        if (!owner || contains_region(*owner, index)) owner = index;
    }
    return owner;
}

std::optional<TemplateSemanticScope> TemplateDefinition::semantic_scope(std::optional<std::size_t> call, std::optional<std::size_t> root) const {
    if (!call) return std::nullopt;
    TemplateSemanticScope result;
    const auto stop = root.value_or(source_call);
    while (*call != stop) {
        const auto region = std::ranges::find(regions, *call, &TemplateRegion::source_call);
        if (region == regions.end()) return std::nullopt;
        if (!region->semantic.empty()) result.path.push_back(region->semantic);
        if (!result.request_slot) result.request_slot = region->request_slot;
        if (!result.input_slot) result.input_slot = region->input_slot;
        if (!result.model_layer)
            for (const auto & interface : collective_interfaces)
                if (interface.model_region && regions[*interface.model_region].source_call == *call) {
                    result.model_layer = interface.model_layer;
                    break;
                }
        call = region->parent_call;
    }
    std::ranges::reverse(result.path);
    return result;
}

TemplateSubgraphRegion TemplateDefinition::complete_region(TemplateSubgraphRegion region, std::optional<TemplateRegionScope> control) const {
    // Membership may come from a native operation window or an instrumented
    // semantic scope. Both use the same dependency and external-port partition.
    std::set<std::size_t> points(region.boundaries.begin(), region.boundaries.end());
    // A task join is internal only when all its recorded contributors are inside.
    // Forward completion remains an external consumer of the operation.
    std::map<std::size_t, std::vector<std::size_t>> task_joins;
    for (const auto & edge : graph.dependencies()) {
        if (edge.relation == SourceRelation::CompletionJoin && lanes[graph.boundary(edge.target).lane].source.domain == SourceDomain::DeviceTaskCompletion)
            task_joins[edge.target].push_back(edge.source);
    }
    for (std::size_t index = 0; index < graph.connection_count(); ++index) {
        const auto connection = this->connection(index);
        if (connection.direction == TemplateConnectionDirection::Incoming) task_joins.erase(connection.local_boundary);
    }
    for (const auto & [target, sources] : task_joins) {
        if (std::ranges::all_of(sources, [&points](auto source) { return points.contains(source); })) points.insert(target);
    }
    region.boundaries.assign(points.begin(), points.end());

    // Endpoint membership does not transfer ownership of an enclosing or adjacent
    // service. Keep its extent outside the replaceable dependency partition.
    std::set<std::pair<std::size_t, std::size_t>> retained_extents;
    for (std::size_t index = 0; index < graph.size(); ++index) {
        if (!std::ranges::binary_search(region.nodes, index)) retained_extents.emplace(graph[index].begin, graph[index].end);
    }

    for (std::size_t index = 0; index < graph.dependencies().size(); ++index) {
        const auto & edge = graph.dependencies()[index];
        const bool source = points.contains(edge.source);
        const bool target = points.contains(edge.target);
        if (!source && !target) continue;
        if (edge.relation == SourceRelation::IntervalExtent && retained_extents.contains({ edge.source, edge.target })) {
            region.retained_dependencies.push_back(index);
            continue;
        }
        if (source && target) region.internal_dependencies.push_back(index);
        else if (target) region.incoming_dependencies.push_back(index);
        else if (source) region.outgoing_dependencies.push_back(index);
    }
    for (std::size_t index = 0; index < graph.connection_count(); ++index) {
        if (points.contains(graph.connection(index).contract.local_boundary)) region.external_connections.push_back(index);
    }
    // Derived model dataflow is separate from observed source edges, but must
    // participate in the same replacement boundary.
    for (std::size_t index = 0; index < rotary_position_dependencies.size(); ++index) {
        const auto & edge = rotary_position_dependencies[index];
        const bool source = std::ranges::binary_search(region.nodes, edge.producer);
        const bool target = std::ranges::binary_search(region.nodes, edge.consumer);
        if (!source && target) region.incoming_rotary_dependencies.push_back(index);
        if (source && !target) region.outgoing_rotary_dependencies.push_back(index);
    }
    region.resource_ports = bind_region_ports(region, control);
    return region;
}

nlohmann::json TemplateResource::describe() const {
    Json result = {
        {    "domain", domain == SourceDomain::CpuThread ? "cpu_thread" : "device_stream" },
        {      "rank",                                                               rank },
        { "native_id",                                                          native_id },
        {      "role",                                                            nullptr }
    };
    if (role) {
        switch (*role) {
        case TemplateResourceRole::Control:
            result["role"] = "control";
            break;
        case TemplateResourceRole::Submission:
            result["role"] = "submission";
            break;
        case TemplateResourceRole::Model:
            result["role"] = "model";
            break;
        case TemplateResourceRole::Collective:
            result["role"] = "collective";
            break;
        }
    }
    return result;
}

bool TemplateDefinition::is_resource_wait_import(const TemplateConnection & connection) const {
    if (connection.relation != SourceRelation::NativeHostWait || connection.direction != TemplateConnectionDirection::Incoming || !connection.remote_order
        || *connection.remote_order >= source_order || !connection.resource)
        return false;

    // A recorded wait owns the target resource query. A source edge alone does
    // not establish the wait scope or justify dropping an external dependency.
    return std::ranges::any_of(stream_waits, [&connection](const auto & wait) {
        return wait.completion == connection.local_boundary && wait.has_resource_scope()
               && std::ranges::find(wait.resources, *connection.resource) != wait.resources.end();
    });
}

bool TemplateDefinition::is_region_wait_import(const TemplateDependency & edge, const TemplateSubgraphRegion & region) const {
    if (edge.relation != SourceRelation::NativeHostWait) return false;
    const auto resource = lanes[graph.boundary(edge.source).lane].resource();
    if (resource.domain != SourceDomain::DeviceStream) return false;

    for (const auto & wait : stream_waits) {
        if (wait.completion != edge.target || !wait.has_resource_scope() || std::ranges::find(wait.resources, resource) == wait.resources.end()) continue;
        // A shared endpoint is not ownership. The whole wait must belong to
        // this cut before its incoming dependency can be removed or rebound.
        for (const auto index : region.nodes) {
            if (graph[index].kind == SourceSpanKind::HostWait && observation(graph[index]).source_span == wait.source_span) return true;
        }
    }
    return false;
}

std::expected<std::vector<TemplateRegionPort>, std::string> TemplateDefinition::bind_region_ports(TemplateSubgraphRegion & region,
                                                                                                  std::optional<TemplateRegionScope> control) const {
    if (!region.crossing_nodes.empty() || !region.incoming_rotary_dependencies.empty() || !region.outgoing_rotary_dependencies.empty())
        region.replacement_gap = "region_crossing_interface_not_supported";
    std::map<TemplateResource, std::set<std::size_t>> entries, exits, completion_exports;
    for (const auto index : region.external_connections) {
        const auto & connection = this->connection(index);
        if (is_resource_wait_import(connection)) continue;
        if (connection.relation == SourceRelation::NativeHostWait && connection.direction == TemplateConnectionDirection::Outgoing && connection.resource
            && connection.resource->domain == SourceDomain::DeviceStream) {
            completion_exports[*connection.resource].insert(connection.local_boundary);
            continue;
        }
        if (!connection.resource
            || !((connection.relation == SourceRelation::ThreadOrder && connection.resource->domain == SourceDomain::CpuThread)
                 || (connection.relation == SourceRelation::StreamOrder && connection.resource->domain == SourceDomain::DeviceStream))) {
            region.replacement_gap = "region_external_relation_not_supported";
            continue; // Data, completion and notification interfaces keep their own crossing records.
        }

        // The producer identifies the stream from the receiving task when the
        // local exit is a completion join. Keep the original connection intact.
        auto & points = connection.direction == TemplateConnectionDirection::Incoming ? entries : exits;
        points[*connection.resource].insert(connection.local_boundary);
    }

    for (const auto index : region.incoming_dependencies) {
        const auto & edge = graph.dependencies()[index];
        if (is_region_wait_import(edge, region)) continue;
        const auto & lane = lanes[graph.boundary(edge.target).lane].resource();
        if (!((edge.relation == SourceRelation::ThreadOrder && lane.domain == SourceDomain::CpuThread)
              || (edge.relation == SourceRelation::StreamOrder && lane.domain == SourceDomain::DeviceStream))) {
            region.replacement_gap = "region_entry_relation_not_supported";
            continue;
        }
        entries[lane].insert(edge.target);
    }
    for (const auto index : region.outgoing_dependencies) {
        const auto & edge = graph.dependencies()[index];
        const auto & producer_lane = lanes[graph.boundary(edge.source).lane].resource();
        if (edge.relation == SourceRelation::NativeHostWait && producer_lane.domain == SourceDomain::DeviceStream) {
            const TemplateResource resource = producer_lane;
            const bool recorded_wait = std::ranges::any_of(stream_waits, [&](const auto & wait) {
                return wait.completion == edge.target && wait.has_resource_scope() && std::ranges::find(wait.resources, resource) != wait.resources.end();
            });
            if (!recorded_wait) return std::unexpected("region_completion_wait_not_observed");

            // A later region can wait on this device completion just as a later
            // template can. Keep the edge and expose its actual producer, then
            // qualify the completion join together with all other exports.
            completion_exports[resource].insert(edge.source);
            continue;
        }

        // A device exit can be a task-completion join. Its stream is identified
        // by the recorded successor, not by the join's synthetic lane identity.
        const auto point = edge.relation == SourceRelation::StreamOrder ? edge.target : edge.source;
        const auto & lane = lanes[graph.boundary(point).lane].resource();
        if (!((edge.relation == SourceRelation::ThreadOrder && lane.domain == SourceDomain::CpuThread)
              || (edge.relation == SourceRelation::StreamOrder && lane.domain == SourceDomain::DeviceStream))) {
            region.replacement_gap = "region_exit_relation_not_supported";
            continue;
        }
        exits[lane].insert(edge.source);
    }
    DependencyPaths<std::size_t> successors;
    for (const auto index : region.internal_dependencies) {
        const auto & edge = graph.dependencies()[index];
        successors.connect(edge.source, edge.target);
    }
    std::map<TemplateResource, std::set<std::size_t>> starts, ends;
    for (const auto index : region.nodes) {
        const auto node = graph[index];
        successors.connect(node.begin, node.end);
        const auto resource = lanes[graph.boundary(node.begin).lane].resource();
        if (resource.domain != SourceDomain::CpuThread && resource.domain != SourceDomain::DeviceStream) continue;
        starts[resource].insert(node.begin);
        ends[resource].insert(node.end);
    }
    const auto predecessors = successors.reversed();
    for (const auto & [resource, points] : starts) {
        if (!entries.contains(resource)) entries.emplace(resource, successors.frontier(points));
        if (!exits.contains(resource)) exits.emplace(resource, predecessors.frontier(ends.at(resource)));
    }
    if (control) {
        const auto resource = lanes[graph.boundary(control->begin).lane].resource();
        entries[resource] = { control->begin };
        exits[resource] = { control->end };
    }

    // Device completion is a property of the work, even when the source has
    // no external host wait. Derive it from causality so another cut can use
    // the same port. A final submission also supplies its missing stream exit.
    for (const auto & [resource, incoming] : entries) {
        if (resource.domain != SourceDomain::DeviceStream && exits.contains(resource)) continue;
        std::set<std::size_t> candidates;
        for (const auto index : region.nodes) {
            const auto & node = graph[index];
            if (node.role() == TemplateNodeRole::Annotation) continue;
            const auto & lane = lanes[graph.boundary(node.begin).lane].resource();
            if (lane == resource) candidates.insert(node.end);
        }

        candidates = predecessors.frontier(std::move(candidates));
        if (candidates.size() != 1) return std::unexpected("region_terminal_work_not_unique");
        const auto exported = completion_exports.find(resource);
        if (exported != completion_exports.end() && exported->second != candidates) region.replacement_gap = "region_external_completion_not_terminal";
        if (resource.domain == SourceDomain::DeviceStream) completion_exports[resource] = candidates;
        exits.emplace(resource, std::move(candidates));
    }
    for (const auto & [resource, points] : completion_exports) {
        if (!entries.contains(resource)) return std::unexpected("region_completion_resource_not_qualified");
    }
    if (entries.empty() || entries.size() != exits.size()) return std::unexpected("region_resource_interface_not_paired");

    std::vector<TemplateRegionPort> result;
    for (const auto & [resource, incoming] : entries) {
        const auto found = exits.find(resource);
        if (incoming.size() != 1 || found == exits.end() || found->second.size() != 1) return std::unexpected("region_resource_interface_not_unique");
        result.push_back({ resource, *incoming.begin(), *found->second.begin() });
        const auto exported = completion_exports.find(resource);
        if (exported != completion_exports.end()) {
            const auto done = *exported->second.begin();
            const auto exit = *found->second.begin();
            // A task-completion alias may carry stream order while the actual
            // device end is exported to a host wait. Prove their zero-work join.
            if (done != exit) {
                const auto previous = predecessors.successors(exit);
                if (previous.size() != 1 || previous.front() != done) return std::unexpected("region_completion_join_not_qualified");
                bool joined = false;
                for (const auto index : region.internal_dependencies) {
                    const auto & edge = graph.dependencies()[index];
                    if (edge.source == done && edge.target == exit && edge.relation == SourceRelation::CompletionJoin) joined = true;
                }
                if (!joined) return std::unexpected("region_completion_join_not_qualified");
            }
            result.back().device_done = done;
        }
    }

    for (const auto index : region.nodes) {
        const auto & node = graph[index];
        if (node.role() == TemplateNodeRole::Annotation) continue;
        const auto & lane = lanes[graph.boundary(node.begin).lane].resource();
        if (!entries.contains(lane)) return std::unexpected("region_work_has_no_resource_port");
    }

    for (const auto & port : result) {
        const auto reached = successors.reachable({ port.entry });
        const auto ancestors = predecessors.reachable({ port.exit });
        if (!reached.contains(port.exit)) return std::unexpected("region_resource_path_disconnected");

        for (const auto index : region.nodes) {
            const auto & node = graph[index];
            if (node.role() == TemplateNodeRole::Annotation) continue;
            const auto & lane = lanes[graph.boundary(node.begin).lane].resource();
            if (lane != port.resource) continue;
            if (!reached.contains(node.begin) || !ancestors.contains(node.end)) return std::unexpected("region_work_outside_resource_path");
        }
    }
    std::set<std::size_t> exposed;
    for (const auto & port : result) {
        exposed.insert(port.entry);
        exposed.insert(port.exit);
        if (port.device_done) exposed.insert(*port.device_done);
    }
    for (const auto index : region.retained_dependencies) {
        const auto edge = graph.dependencies()[index];
        if ((std::ranges::binary_search(region.boundaries, edge.source) && !exposed.contains(edge.source))
            || (std::ranges::binary_search(region.boundaries, edge.target) && !exposed.contains(edge.target)))
            region.replacement_gap = "region_retained_work_has_internal_endpoint";
    }
    for (const auto & [port, point] : ports)
        if (std::ranges::binary_search(region.boundaries, point)) region.replacement_gap = "region_contains_serving_port";

    return result;
}

std::expected<std::vector<TemplateRegionPort>, std::string> TemplateDefinition::region_ports(const TemplateSubgraphRegion & region) const {
    if (region.replacement_gap) return std::unexpected(*region.replacement_gap);
    return region.resource_ports;
}


std::expected<TemplateDeviceOperationPorts, std::string> TemplateDefinition::device_operation_ports(const TemplateOperation & operation,
                                                                                                    const TemplateSubgraphRegion & region) const {
    if (operation.device_spans.empty() || !region.crossing_nodes.empty()) return std::unexpected("operation_boundary_not_supported");

    std::set<std::size_t> entries;
    std::set<std::size_t> exits;
    for (const auto index : region.incoming_dependencies) {
        const auto & edge = graph.dependencies()[index];
        if (edge.relation == SourceRelation::StreamOrder) entries.insert(edge.target);
    }
    for (const auto index : region.outgoing_dependencies) {
        const auto & edge = graph.dependencies()[index];
        if (edge.relation == SourceRelation::StreamOrder) exits.insert(edge.source);
    }
    // The stream successor can belong to the next serving block. It remains
    // an operation port even when represented by a cross-template connection.
    for (const auto index : region.external_connections) {
        const auto & connection = this->connection(index);
        if (connection.relation != SourceRelation::StreamOrder) continue;
        auto & points = connection.direction == TemplateConnectionDirection::Incoming ? entries : exits;
        points.insert(connection.local_boundary);
    }
    // An operation can own the first observed task on an initially idle stream.
    // It still has a device entry, even though no predecessor crosses its cut.
    // Internal stream edges exclude later tasks; the reachability check below
    // requires all device work to belong to the same serial operation.
    if (entries.empty()) {
        for (const auto index : region.nodes) {
            const auto & node = graph[index];
            if (std::ranges::find(operation.device_spans, observation(node).source_span) != operation.device_spans.end()) entries.insert(node.begin);
        }
        for (const auto index : region.internal_dependencies) {
            const auto & edge = graph.dependencies()[index];
            if (edge.relation == SourceRelation::StreamOrder) entries.erase(edge.target);
        }
    }
    if (entries.size() != 1 || exits.size() != 1) return std::unexpected("operation_stream_cut_not_unique");
    const auto entry = *entries.begin();
    const auto exit = *exits.begin();
    if (lanes[graph.boundary(entry).lane].source.domain != SourceDomain::DeviceStream
        || lanes[graph.boundary(exit).lane].source.domain != SourceDomain::DeviceTaskCompletion)
        return std::unexpected("operation_stream_cut_not_qualified");

    std::set<std::size_t> device_ends;
    for (const auto index : region.nodes) {
        const auto & node = graph[index];
        if (std::ranges::find(operation.device_spans, observation(node).source_span) != operation.device_spans.end()) device_ends.insert(node.end);
    }
    DependencyPaths<std::size_t> predecessors;
    std::set<std::size_t> terminal;
    for (const auto index : region.internal_dependencies) {
        const auto & edge = graph.dependencies()[index];
        predecessors.connect(edge.target, edge.source);
        if (edge.relation == SourceRelation::CompletionJoin && edge.target == exit && device_ends.contains(edge.source)) terminal.insert(edge.source);
    }
    if (terminal.size() != 1) return std::unexpected("operation_device_completion_not_unique");
    const auto done = *terminal.begin();

    // Every recorded device completion must precede the exported final work.
    // This is a graph condition, never a comparison of observed timestamps.
    const auto ancestors = predecessors.reachable({ done });
    for (const auto point : device_ends) {
        if (!ancestors.contains(point)) return std::unexpected("operation_device_order_not_qualified");
    }
    if (!ancestors.contains(entry)) return std::unexpected("operation_device_entry_not_connected");

    std::set<std::size_t> cpu_entries{ operation.host.begin };
    std::set<std::size_t> cpu_exits{ operation.host.end };
    for (const auto & worker : operation.workers) {
        cpu_entries.insert(worker.begin);
        cpu_exits.insert(worker.end);
    }
    std::set<std::size_t> completion_targets;
    for (const auto index : region.outgoing_dependencies) {
        const auto & edge = graph.dependencies()[index];
        if (edge.relation == SourceRelation::CompletionJoin && edge.source == done
            && lanes[graph.boundary(edge.target).lane].source.domain == SourceDomain::LogicalCompletion)
            completion_targets.insert(edge.target);
    }
    for (const auto index : region.incoming_dependencies) {
        const auto & edge = graph.dependencies()[index];
        if (edge.relation == SourceRelation::StreamOrder && edge.target == entry) continue;
        if (edge.relation == SourceRelation::ThreadOrder && cpu_entries.contains(edge.target)) continue;
        return std::unexpected("operation_external_entry_not_qualified");
    }
    for (const auto index : region.outgoing_dependencies) {
        const auto & edge = graph.dependencies()[index];
        if (edge.relation == SourceRelation::StreamOrder && edge.source == exit) continue;
        if (edge.relation == SourceRelation::ThreadOrder && cpu_exits.contains(edge.source)) continue;
        if (edge.relation == SourceRelation::NativeHostWait && edge.source == done) continue;
        // An intermediate completion can share the final join only when that
        // consumer already waits for the final work. Otherwise retain a separate port.
        if (edge.relation == SourceRelation::CompletionJoin && device_ends.contains(edge.source) && completion_targets.contains(edge.target)) continue;
        return std::unexpected("operation_external_completion_not_qualified");
    }
    for (const auto index : region.external_connections) {
        const auto & connection = this->connection(index);
        const bool incoming = connection.direction == TemplateConnectionDirection::Incoming;
        const auto & cpu_ports = incoming ? cpu_entries : cpu_exits;
        if (connection.relation == SourceRelation::ThreadOrder && cpu_ports.contains(connection.local_boundary)) continue;
        if (connection.relation == SourceRelation::StreamOrder && connection.local_boundary == (incoming ? entry : exit)) continue;
        if (!incoming && connection.relation == SourceRelation::NativeHostWait && connection.local_boundary == done) continue;
        return std::unexpected("operation_template_port_not_qualified");
    }
    return TemplateDeviceOperationPorts{ entry, exit, done };
}


std::expected<TemplateOperationRewrite, std::string> TemplateDefinition::plan_view_materialization_removal(const TemplateOperation & operation,
                                                                                                           std::optional<std::size_t> view_producer) const {
    const auto region = operation_region(operation);
    if (operation.device_spans.size() != 1 || !region.crossing_nodes.empty()) return std::unexpected("position_operation_boundary_not_supported");
    if (!region.incoming_rotary_dependencies.empty() || region.outgoing_rotary_dependencies.empty())
        return std::unexpected("position_shared_consumers_not_qualified");
    if (!view_producer) return std::unexpected("position_view_producer_missing");

    const auto device =
        std::find_if(graph.begin(), graph.end(), [&](const auto & node) { return observation(node).source_span == operation.device_spans.front(); });
    if (device == graph.end()) return std::unexpected("operation_device_node_missing");
    const auto producer = device.position;
    const auto device_ports = device_operation_ports(operation, region);
    if (!device_ports) return std::unexpected(device_ports.error());
    TemplateOperationRewrite removal;
    removal.nodes = region.nodes;
    const std::set<std::size_t> points(region.boundaries.begin(), region.boundaries.end());
    std::set<std::size_t> cpu_entries{ operation.host.begin };
    std::set<std::size_t> cpu_exits{ operation.host.end };
    removal.added_dependencies.push_back({ SourceRelation::ThreadOrder, operation.host.begin, operation.host.end });
    for (const auto & window : operation.workers) {
        cpu_entries.insert(window.begin);
        cpu_exits.insert(window.end);
        removal.added_dependencies.push_back({ SourceRelation::ThreadOrder, window.begin, window.end });
    }
    for (const auto index : region.nodes) {
        const auto kind = graph[index].kind;
        if (kind != SourceSpanKind::CpuObservation && kind != SourceSpanKind::CpuControl && kind != SourceSpanKind::Compute)
            return std::unexpected("operation_has_other_work");
    }
    for (const auto & [port, boundary] : ports) {
        if (points.contains(boundary)) return std::unexpected("operation_contains_serving_port");
    }

    for (std::size_t index = 0; index < graph.dependencies().size(); ++index) {
        const auto & edge = graph.dependencies()[index];
        const bool source = points.contains(edge.source);
        const bool target = points.contains(edge.target);
        if (!source && !target) continue;
        if (std::ranges::binary_search(region.retained_dependencies, index)) continue;
        if (source && target) {
            if (edge.relation != SourceRelation::IntervalExtent && edge.relation != SourceRelation::ThreadOrder && edge.relation != SourceRelation::NativeFlow
                && edge.relation != SourceRelation::CompletionJoin)
                return std::unexpected("operation_internal_dependency_not_supported");
            removal.dependencies.push_back(index);
            continue;
        }
        if (edge.relation == SourceRelation::ThreadOrder && ((!source && cpu_entries.contains(edge.target)) || (!target && cpu_exits.contains(edge.source))))
            continue;
        if (edge.relation == SourceRelation::StreamOrder) continue;
        if (source && edge.relation == SourceRelation::CompletionJoin && edge.source == graph[producer].end
            && lanes[graph.boundary(edge.target).lane].source.domain == SourceDomain::LogicalCompletion) {
            removal.dependencies.push_back(index);
            continue;
        }
        return std::unexpected("operation_external_dependency_not_supported");
    }
    if (device_ports->stream_entry != graph[producer].begin || device_ports->device_done != graph[producer].end)
        return std::unexpected("operation_stream_cut_not_qualified");
    removal.added_dependencies.push_back({ SourceRelation::StreamOrder, device_ports->stream_entry, device_ports->stream_exit });

    // These connections survive unchanged, including CPU thread projection.
    // Applying the plan must keep their bound target endpoints, not raw times.
    for (const auto index : region.external_connections) {
        const auto & edge = this->connection(index);
        const auto & endpoints = edge.direction == TemplateConnectionDirection::Incoming ? cpu_entries : cpu_exits;
        if (edge.relation != SourceRelation::ThreadOrder || !endpoints.contains(edge.local_boundary))
            return std::unexpected("operation_template_connection_not_supported");
    }
    removal.rotary_dependencies = region.outgoing_rotary_dependencies;
    removal.rotary_producer = *view_producer;
    return removal;
}

std::partial_ordering TemplateDefinition::region_order(const TemplateRegion & first, const TemplateRegion & second) const {
    if (first.source_call == second.source_call || first.parent_call != second.parent_call) return std::partial_ordering::unordered;
    if (std::ranges::any_of(first.nodes, [&](const auto index) { return std::ranges::binary_search(second.nodes, index); }))
        return std::partial_ordering::unordered;
    const auto first_ports = region_ports(first);
    const auto second_ports = region_ports(second);
    if (!first_ports || !second_ports) return std::partial_ordering::unordered;

    // Qualify both directions from one pair of cuts. Only a native path on
    // the same CPU lane establishes order; independent regions stay unordered.
    DependencyPaths<std::size_t> paths;
    for (const auto & edge : graph.dependencies()) {
        if (edge.relation != SourceRelation::ThreadOrder && edge.relation != SourceRelation::IntervalExtent) continue;
        const auto lane = graph.boundary(edge.source).lane;
        if (graph.boundary(edge.target).lane == lane && lanes[lane].source.domain == SourceDomain::CpuThread) paths.connect(edge.source, edge.target);
    }

    bool before = false, after = false;
    for (const auto & entry : *second_ports) {
        if (entry.resource.domain != SourceDomain::CpuThread) continue;
        const auto previous = std::ranges::find(*first_ports, entry.resource, &TemplateRegionPort::resource);
        if (previous == first_ports->end()) continue;
        before = before || paths.reachable({ previous->exit }).contains(entry.entry);
        after = after || paths.reachable({ entry.exit }).contains(previous->entry);
    }
    if (before == after) return std::partial_ordering::unordered;
    return before ? std::partial_ordering::less : std::partial_ordering::greater;
}

std::expected<std::optional<TemplateRegionResourceAnchor>, std::string> TemplateDefinition::region_resource_anchor(const TemplateRegion & region,
                                                                                                                   const TemplateResource & resource) const {
    const auto & recipient = region;
    std::vector<std::size_t> outside;
    for (std::size_t index = 0; index < graph.size(); ++index) {
        const auto & node = graph[index];
        if (node.role() == TemplateNodeRole::Annotation || std::ranges::binary_search(recipient.nodes, index)) continue;
        const auto & lane = lanes[graph.boundary(node.begin).lane].resource();
        if (lane == resource) outside.push_back(index);
    }
    if (outside.empty()) return std::nullopt;

    std::optional<TemplateRegionResourceAnchor> anchor;
    for (const auto & candidate : regions) {
        if (candidate.source_call == region.source_call || candidate.parent_call != region.parent_call) continue;
        const auto & cut = candidate;
        if (!std::ranges::includes(cut.nodes, outside)) continue;
        if (std::ranges::any_of(cut.nodes, [&](const auto index) { return std::ranges::binary_search(recipient.nodes, index); })) continue;
        const auto ports = region_ports(cut);
        if (!ports) continue;
        const auto resource_port = std::ranges::find(*ports, resource, &TemplateRegionPort::resource);
        if (resource_port == ports->end()) continue;

        const auto order = region_order(candidate, region);
        if (order == std::partial_ordering::unordered) continue;
        if (anchor) return std::unexpected("region_resource_anchor_not_unique");
        anchor = TemplateRegionResourceAnchor{ *resource_port, static_cast<std::size_t>(&candidate - regions.data()), order < 0 };
    }
    if (!anchor) return std::unexpected("region_new_resource_has_unexposed_work");
    return anchor;
}

std::expected<TemplateOperationRewrite, std::string>
    TemplateDefinition::plan_region_replacement(const TemplateRegion & region, const TemplateDefinition & donor, const TemplateRegion & donor_region) const {
    // A qualified transition ends before selecting Prefill or Decode. Only
    // that common prefix can cross the enclosing batch's observed phase.
    const bool transition_prefix =
        transition_region && donor.transition_region && &region == &regions[*transition_region] && &donor_region == &donor.regions[*donor.transition_region];
    if (rank != donor.rank || step != donor.step || (phase != donor.phase && !transition_prefix)) return std::unexpected("region_context_not_matched");
    return plan_subgraph_replacement(region, donor, donor_region, &region, donor_region.source_call);
}

std::expected<TemplateOperationRewrite, std::string> TemplateDefinition::plan_workspace_replacement(const TemplateWorkspaceUse & use,
                                                                                                    const TemplateDefinition & donor,
                                                                                                    const TemplateWorkspaceUse & donor_use) const {
    if (rank != donor.rank || step != donor.step || phase != donor.phase) return std::unexpected("workspace_context_not_matched");
    return plan_subgraph_replacement(workspace_region(use), donor, donor.workspace_region(donor_use), nullptr);
}

std::expected<TemplateOperationRewrite, std::string> TemplateDefinition::plan_subgraph_replacement(const TemplateSubgraphRegion & recipient_cut,
                                                                                                   const TemplateDefinition & donor,
                                                                                                   const TemplateSubgraphRegion & donor_cut,
                                                                                                   const TemplateRegion * region,
                                                                                                   std::optional<std::size_t> donor_root) const {
    if (source_manifest != donor.source_manifest && (!has_resource_roles() || !donor.has_resource_roles()))
        return std::unexpected("cross_source_resource_role_not_qualified");

    const auto recipient_ports = region_ports(recipient_cut);
    const auto donor_ports = donor.region_ports(donor_cut);
    if (!recipient_ports) return std::unexpected(recipient_ports.error());
    if (!donor_ports) return std::unexpected(donor_ports.error());

    TemplateOperationRewrite result;
    if (region) result.source_region = static_cast<std::size_t>(region - regions.data());
    std::map<std::size_t, std::size_t> points, lane_mapping;
    std::map<TemplateResource, TemplateRegionResourceAnchor> resource_anchors;
    std::optional<std::size_t> completion_lane;
    for (const auto & from : *donor_ports) {
        const auto found = std::ranges::find(*recipient_ports, from.resource, &TemplateRegionPort::resource);
        if (found == recipient_ports->end()) {
            if (!region) return std::unexpected("subgraph_new_resource_anchor_not_qualified");
            const auto anchor = region_resource_anchor(*region, from.resource);
            if (!anchor) return std::unexpected(anchor.error());
            if (*anchor) resource_anchors.emplace(from.resource, **anchor);
            for (const auto boundary : { from.entry, from.exit }) {
                const auto lane = donor.graph.boundary(boundary).lane;
                if (!lane_mapping.contains(lane)) {
                    lane_mapping.emplace(lane, lanes.size() + result.inserted_lanes.size());
                    result.inserted_lanes.push_back(donor.lanes[lane]);
                }
            }
            if ((from.resource).domain == SourceDomain::DeviceStream) {
                if (completion_lane || donor.lanes[donor.graph.boundary(from.exit).lane].source.domain != SourceDomain::DeviceTaskCompletion)
                    return std::unexpected("region_serial_completion_not_qualified");
                completion_lane = lane_mapping.at(donor.graph.boundary(from.exit).lane);
            }
            result.added_resource_ports.push_back(from);
            continue;
        }
        // Recipient consumers require their completion export. A donor-only
        // export stays internal when this target cut has no such consumer.
        if (found->device_done && !from.device_done) return std::unexpected("region_completion_interface_not_matched");
        auto target_exit = found->exit;
        if (from.device_done && found->device_done) {
            if (from.exit != *from.device_done && found->exit == *found->device_done) return std::unexpected("region_completion_interface_not_matched");

            // A final source task can have no stream-order alias. Preserve the
            // recipient's proven zero-work join for its existing consumers.
            if (from.exit == *from.device_done && found->exit != *found->device_done) {
                target_exit = *found->device_done;
                result.added_dependencies.push_back({ SourceRelation::CompletionJoin, target_exit, found->exit });
            }
            points.emplace(*from.device_done, *found->device_done);
        }
        points.emplace(from.entry, found->entry);
        points.emplace(from.exit, target_exit);
        lane_mapping.emplace(donor.graph.boundary(from.entry).lane, graph.boundary(found->entry).lane);
        lane_mapping.emplace(donor.graph.boundary(from.exit).lane, graph.boundary(target_exit).lane);
        if ((from.resource).domain == SourceDomain::DeviceStream) {
            if (completion_lane || lanes[graph.boundary(found->exit).lane].source.domain != SourceDomain::DeviceTaskCompletion)
                return std::unexpected("region_serial_completion_not_qualified");
            completion_lane = graph.boundary(found->exit).lane;
        }
    }
    for (const auto point : donor_cut.boundaries) {
        const auto lane = donor.graph.boundary(point).lane;
        if (lane_mapping.contains(lane)) continue;
        // Internal joins have target-local identity; they introduce no resource.
        if (completion_lane && donor.lanes[lane].source.domain == SourceDomain::DeviceTaskCompletion) lane_mapping.emplace(lane, *completion_lane);
        else return std::unexpected("region_additional_resource_not_supported");
    }

    // Wait inputs belong to the removed region, not to the resource port.
    // A retained donor wait gets the target cut's predecessor below; its old
    // source completion must never leak into the target instance.
    std::vector<std::pair<std::size_t, std::size_t>> wait_predecessors;
    for (const auto index : donor_cut.incoming_dependencies) {
        const auto & edge = donor.graph.dependencies()[index];
        if (!donor.is_region_wait_import(edge, donor_cut)) continue;
        const auto & lane = donor.lanes[donor.graph.boundary(edge.source).lane].resource();
        const TemplateResource resource = lane;
        const auto port = std::ranges::find(*recipient_ports, resource, &TemplateRegionPort::resource);
        if (port == recipient_ports->end()) {
            const auto previous = resource_anchors.find(resource);
            // A CPU-only cut exposes no device port. Its scoped wait is bound
            // from actual target submissions after all topology rewrites.
            if (previous == resource_anchors.end() || !previous->second.predecessor) continue;
            const auto & port = previous->second.port;
            wait_predecessors.emplace_back(edge.target, port.device_done.value_or(port.exit));
            continue;
        }

        std::optional<std::size_t> predecessor;
        for (const auto incoming : recipient_cut.incoming_dependencies) {
            const auto & candidate = graph.dependencies()[incoming];
            if (candidate.relation != SourceRelation::StreamOrder || candidate.target != port->entry) continue;
            if (predecessor && *predecessor != candidate.source) return std::unexpected("region_wait_predecessor_not_unique");
            predecessor = candidate.source;
        }
        if (!predecessor) return std::unexpected("region_wait_predecessor_not_exposed");
        wait_predecessors.emplace_back(edge.target, *predecessor);
    }

    result.nodes = recipient_cut.nodes;
    result.dependencies = recipient_cut.internal_dependencies;
    for (const auto index : recipient_cut.incoming_dependencies) {
        if (is_region_wait_import(graph.dependencies()[index], recipient_cut)) result.dependencies.push_back(index);
    }
    for (const auto & port : *recipient_ports) {
        if (std::ranges::find(*donor_ports, port.resource, &TemplateRegionPort::resource) != donor_ports->end()) continue;

        // Keep both exported completion identities as ordering points, without
        // creating a service or claiming that a new device task was submitted.
        const auto relation = (port.resource).domain == SourceDomain::CpuThread ? SourceRelation::ThreadOrder : SourceRelation::StreamOrder;
        const auto done = port.device_done.value_or(port.exit);
        result.added_dependencies.push_back({ relation, port.entry, done });
        if (done != port.exit) result.added_dependencies.push_back({ SourceRelation::CompletionJoin, done, port.exit });
    }
    const auto recipient_owner = region_owner(recipient_cut.nodes);
    const auto donor_owner = donor.region_owner(donor_cut.nodes);
    const auto roots = std::pair{ donor_root.value_or(donor_owner ? donor.regions[*donor_owner].source_call : donor.source_call),
                                  region            ? region->source_call
                                  : recipient_owner ? regions[*recipient_owner].source_call
                                                    : source_call };
    const auto mapping = result.insert_subgraph(donor, *this, donor_cut.nodes, donor_cut.internal_dependencies, std::move(points), lane_mapping, roots);
    for (const auto & [completion, predecessor] : wait_predecessors)
        result.added_dependencies.push_back({ SourceRelation::NativeHostWait, predecessor, mapping.at(completion) });
    for (auto & port : result.added_resource_ports) {
        const auto anchor = resource_anchors.find(port.resource);
        if (anchor != resource_anchors.end()) {
            const auto relation = (port.resource).domain == SourceDomain::CpuThread ? SourceRelation::ThreadOrder : SourceRelation::StreamOrder;
            if (anchor->second.predecessor) result.added_dependencies.push_back({ relation, anchor->second.port.exit, mapping.at(port.entry) });
            else {
                result.added_dependencies.push_back({ relation, mapping.at(port.exit), anchor->second.port.entry });

                // Stream queries inside the following region also see the new
                // submission. Existing previous-batch bindings alone miss it.
                const auto & successor = regions[anchor->second.region];
                for (const auto & wait : stream_waits) {
                    if (!wait.has_resource_scope() || std::ranges::find(wait.resources, port.resource) == wait.resources.end()) continue;
                    const bool owned = std::ranges::any_of(successor.nodes, [&](const auto index) {
                        return graph[index].kind == SourceSpanKind::HostWait && observation(graph[index]).source_span == wait.source_span;
                    });
                    if (owned)
                        result.added_dependencies.push_back(
                            { SourceRelation::NativeHostWait, mapping.at(port.device_done.value_or(port.exit)), wait.completion });
                }
            }
        }
        port.entry = mapping.at(port.entry);
        port.exit = mapping.at(port.exit);
        if (port.device_done) port.device_done = mapping.at(*port.device_done);
    }
    return result;
}

std::expected<TemplateOperationRewrite, std::string> TemplateDefinition::plan_operation_replacement(const TemplateOperation & operation,
                                                                                                    const TemplateDefinition & donor,
                                                                                                    const TemplateOperation & donor_operation) const {
    if (source_manifest != donor.source_manifest && (!has_resource_roles() || !donor.has_resource_roles()))
        return std::unexpected("cross_source_resource_role_not_qualified");

    const auto source_region = operation_region(operation);
    const auto donor_region = donor.operation_region(donor_operation);
    if (rank != donor.rank || !donor_region.crossing_nodes.empty() || !source_region.incoming_rotary_dependencies.empty()
        || !source_region.outgoing_rotary_dependencies.empty() || !donor_region.incoming_rotary_dependencies.empty()
        || !donor_region.outgoing_rotary_dependencies.empty())
        return std::unexpected("operation_replacement_boundary_not_supported");

    // Semantic branch eligibility belongs to the caller. Preserve the source
    // queue/stream interface even when the target operation is a CPU-only view.
    const auto source_ports = device_operation_ports(operation, source_region);
    if (!source_ports) return std::unexpected(source_ports.error());
    for (const auto & [port, boundary] : ports) {
        if (std::ranges::binary_search(source_region.boundaries, boundary)) return std::unexpected("operation_contains_serving_port");
    }
    for (const auto & [port, boundary] : donor.ports) {
        if (std::ranges::binary_search(donor_region.boundaries, boundary)) return std::unexpected("operation_contains_serving_port");
    }

    TemplateOperationRewrite replacement;
    replacement.nodes = source_region.nodes;
    replacement.dependencies = source_region.internal_dependencies;
    const auto recipient_owner = region_owner(source_region.nodes);
    const auto donor_owner = donor.region_owner(donor_region.nodes);
    const auto scope_roots = std::pair{ donor_owner ? donor.regions[*donor_owner].source_call : donor.source_call,
                                        recipient_owner ? regions[*recipient_owner].source_call : source_call };
    if (donor_operation.device_spans.empty()) {
        if (!donor.operation_view_service(donor_operation)) return std::unexpected("operation_cpu_view_not_qualified");
        replacement.insert_subgraph(donor,
                                    *this,
                                    donor_region.nodes,
                                    donor_region.internal_dependencies,
                                    {
                                        { donor_operation.host.begin, operation.host.begin },
                                        {   donor_operation.host.end,   operation.host.end }
        },
                                    { { donor.graph.boundary(donor_operation.host.begin).lane, graph.boundary(operation.host.begin).lane } },
                                    scope_roots);

        // A metadata view submits no device or worker work. Bypass those old
        // extents while retaining their externally referenced order frontiers.
        // Do not make the CPU view wait for the gathered device data.
        for (const auto & worker : operation.workers) replacement.added_dependencies.push_back({ SourceRelation::ThreadOrder, worker.begin, worker.end });
        if (source_ports->stream_entry != source_ports->device_done)
            replacement.added_dependencies.push_back({ SourceRelation::StreamOrder, source_ports->stream_entry, source_ports->device_done });
        if (source_ports->device_done != source_ports->stream_exit)
            replacement.added_dependencies.push_back({ SourceRelation::CompletionJoin, source_ports->device_done, source_ports->stream_exit });
    }
    else {
        const auto donor_ports = donor.device_operation_ports(donor_operation, donor_region);
        if (!donor_ports) return std::unexpected(donor_ports.error());
        // The supported queued path has one worker role, so it needs no ordering
        // heuristic to pair unrelated native threads from different observations.
        if (operation.workers.size() != donor_operation.workers.size() || operation.workers.size() > 1)
            return std::unexpected("operation_worker_mapping_not_supported");
        std::map<std::size_t, std::size_t> boundary_mapping{
            { donor_operation.host.begin,       operation.host.begin },
            {   donor_operation.host.end,         operation.host.end },
            {  donor_ports->stream_entry, source_ports->stream_entry },
            {   donor_ports->stream_exit,  source_ports->stream_exit },
            {   donor_ports->device_done,  source_ports->device_done }
        };
        std::map<std::size_t, std::size_t> lane_mapping{
            { donor.graph.boundary(donor_operation.host.begin).lane,       graph.boundary(operation.host.begin).lane },
            {  donor.graph.boundary(donor_ports->stream_entry).lane, graph.boundary(source_ports->stream_entry).lane }
        };
        for (std::size_t index = 0; index < operation.workers.size(); ++index) {
            const auto & from = donor_operation.workers[index];
            const auto & to = operation.workers[index];
            boundary_mapping.emplace(from.begin, to.begin);
            boundary_mapping.emplace(from.end, to.end);
            lane_mapping.emplace(donor.graph.boundary(from.begin).lane, graph.boundary(to.begin).lane);
        }
        for (const auto point : donor_region.boundaries) {
            const auto lane = donor.graph.boundary(point).lane;
            // Task completion lanes label local joins, not a new physical resource.
            if (donor.lanes[lane].source.domain == SourceDomain::DeviceTaskCompletion)
                lane_mapping.emplace(lane, graph.boundary(source_ports->stream_exit).lane);
            if (!lane_mapping.contains(lane)) return std::unexpected("operation_additional_resource_not_supported");
        }

        replacement
            .insert_subgraph(donor, *this, donor_region.nodes, donor_region.internal_dependencies, std::move(boundary_mapping), lane_mapping, scope_roots);
    }

    // Keep old intermediate completion identities addressable by their existing
    // consumers. Port qualification proves those consumers already await done.
    std::set<std::size_t> preserved_completions;
    for (const auto index : source_region.outgoing_dependencies) {
        const auto & edge = graph.dependencies()[index];
        if (edge.relation == SourceRelation::CompletionJoin && edge.source != source_ports->device_done) preserved_completions.insert(edge.source);
    }
    for (const auto point : preserved_completions)
        replacement.added_dependencies.push_back({ SourceRelation::CompletionJoin, source_ports->device_done, point });
    return replacement;
}

bool TemplateDefinition::has_resource_roles() const {
    for (const auto & lane : lanes)
        if ((lane.source.domain == SourceDomain::CpuThread || lane.source.domain == SourceDomain::DeviceStream) && !lane.role) return false;
    for (const auto & wait : stream_waits)
        for (const auto & resource : wait.resources)
            if (!resource.role) return false;
    for (const auto & connection : connection_observations)
        if (connection.resource && !connection.resource->role) return false;
    return true;
}

bool TemplateDefinition::is_cpu_region(const TemplateRegion & region) const { return is_cpu_region(region.nodes); }

bool TemplateDefinition::is_cpu_region(std::span<const std::size_t> nodes) const {
    std::optional<std::size_t> cpu_lane;
    for (const auto index : nodes) {
        const auto & node = graph[index];
        if (node.kind != SourceSpanKind::CpuObservation && node.kind != SourceSpanKind::ResourceObservation && node.kind != SourceSpanKind::CpuControl)
            return false;
        const auto lane = graph.boundary(node.begin).lane;
        if (lanes[lane].source.domain != SourceDomain::CpuThread || graph.boundary(node.end).lane != lane || (cpu_lane && *cpu_lane != lane)) return false;
        cpu_lane = lane;
    }
    return cpu_lane.has_value();
}

bool TemplateDefinition::matches_transition(const BatchInput & target) const {
    const auto count = target.transition.pending_prefill_requests ? 1U : 0U;
    auto entry = target.transition;
    // The producer qualifies independence from the incoming latch. Preserve
    // membership/merge work here; admission checks the target state separately.
    if (transition_latch_parameterized) entry.admission_blocked = transition->admission_blocked;
    if (transition == entry && merge == target.merge) {
        if (!transition_region) return filters == target.filters;
        if (std::ranges::equal(std::span(filters).first(count), std::span(target.filters).first(count))) return true;
    }

    // Both row counts are parameters of the qualified nonempty merge. The
    // pending filter must still keep every member; the running filter is separate.
    if (transition_merge_parameters && target.merge && target.merge->left_count > 0 && target.merge->right_count > 0
        && target.transition.running_requests == target.merge->left_count && target.transition.pending_prefill_requests == target.merge->right_count) {
        const auto & pending = target.filters.front();
        if (pending.input_count == target.merge->right_count && pending.retained_indices.size() == pending.input_count) return true;
    }

    // Empty/all-kept CPU filters scan target members without creating device
    // indices. Preserve whether the prefix runs, not its observed loop count.
    if (!transition_cpu_parameters || target.merge
        || transition->pending_prefill_requests.has_value() != target.transition.pending_prefill_requests.has_value())
        return false;
    if (count == 0) return true;
    const auto & filter = target.filters.front();
    return filter.retained_indices.empty() || filter.retained_indices.size() == filter.input_count;
}

bool TemplateDefinition::matches_partial_filter(BatchFilterBinding source_role, BatchFilterBinding target_role, const BatchInput & target) const {
    const auto region = filter_regions.find(source_role);
    if (region == filter_regions.end()) return false;

    const auto & observed = filters[source_role == BatchFilterBinding::Transition ? 0 : (transition->pending_prefill_requests ? 1 : 0)];
    const auto & requested = target.filters[target_role == BatchFilterBinding::Transition ? 0 : (target.transition.pending_prefill_requests ? 1 : 0)];
    if (observed.retained_indices.empty() || observed.retained_indices.size() == observed.input_count || requested.retained_indices.empty()
        || requested.retained_indices.size() == requested.input_count)
        return false;

    bool indexed = false;
    for (const auto index : regions[region->second].nodes) {
        const auto & node = graph[index];
        const auto & binding = observation(node);
        if (binding.transfer) {
            const auto layout = transfer_layouts.find(binding.source_span);
            if (layout == transfer_layouts.end()) return false;
            auto rebound = layout->second;
            if (rebound.extent != TransferExtent::TransitionRetainedRequests && rebound.extent != TransferExtent::RunningRetainedRequests) return false;
            rebound.extent =
                target_role == BatchFilterBinding::Transition ? TransferExtent::TransitionRetainedRequests : TransferExtent::RunningRetainedRequests;
            if (!rebound.evaluate(*binding.transfer, target)) return false;
            continue;
        }
        if (node.kind == SourceSpanKind::CpuObservation || node.kind == SourceSpanKind::CpuControl || node.kind == SourceSpanKind::HostWait) continue;
        if (node.kind != SourceSpanKind::Compute || !binding.kernel) return false;
        const auto layout = batch_kernel_layouts.find(binding.source_span);
        if (layout == batch_kernel_layouts.end() || !layout->second) return false;
        const auto kernel = layout->second->with_filter_binding(target_role).evaluate(*binding.kernel, target);
        if (!kernel || kernel->batch_filter != requested) return false;
        indexed = true;
    }
    return indexed;
}

bool TemplateDefinition::matches_running_filter(const BatchInput & target) const {
    if (!running_update_region) return false;
    const auto & observed = filters.back();
    const auto & requested = target.filters.back();
    if (observed == requested) return true;

    // An all-kept filter returns before creating device indices. A qualified
    // CPU-only update keeps that branch when the target membership grows.
    if (!requested.retained_indices.empty() && observed.retained_indices.size() == observed.input_count
        && requested.retained_indices.size() == requested.input_count && is_cpu_region(regions[*running_update_region]))
        return true;

    const auto filter = filter_regions.find(BatchFilterBinding::RunningUpdate);
    if (filter == filter_regions.end()) return false;
    // The common helper does not qualify other device work in the enclosing
    // update. Preserve the original CPU/wait-only shell restriction.
    for (const auto index : regions[*running_update_region].nodes) {
        if (std::ranges::binary_search(regions[filter->second].nodes, index)) continue;
        const auto kind = graph[index].kind;
        if (kind != SourceSpanKind::CpuObservation && kind != SourceSpanKind::CpuControl && kind != SourceSpanKind::HostWait) return false;
    }
    return matches_partial_filter(BatchFilterBinding::RunningUpdate, BatchFilterBinding::RunningUpdate, target);
}

} // namespace markov
