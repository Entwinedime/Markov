#include "markov/templates/templates.hpp"

#include "markov/graph/kernel_shapes.hpp"

#include <algorithm>
#include <cstddef>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>

namespace markov {
namespace {

using Json = nlohmann::json;

SourceSpanKind span_kind(std::string_view name) {
    if (name == "cpu_observation") return SourceSpanKind::CpuObservation;
    if (name == "cpu_control") return SourceSpanKind::CpuControl;
    if (name == "host_wait") return SourceSpanKind::HostWait;
    if (name == "compute") return SourceSpanKind::Compute;
    if (name == "dma") return SourceSpanKind::Dma;
    if (name == "device_wait") return SourceSpanKind::DeviceWait;
    if (name == "device_marker") return SourceSpanKind::DeviceMarker;
    if (name == "communication_observation") return SourceSpanKind::CommunicationObservation;
    throw std::invalid_argument("unknown source span kind: " + std::string(name));
}


} // namespace

TemplateNodeRole TemplateNode::role() const {
    switch (kind) {
    case SourceSpanKind::CpuControl:
    case SourceSpanKind::Compute:
    case SourceSpanKind::Dma:
    case SourceSpanKind::DeviceMarker:
        return TemplateNodeRole::Service;
    case SourceSpanKind::HostWait:
    case SourceSpanKind::DeviceWait:
        return TemplateNodeRole::Wait;
    case SourceSpanKind::CpuObservation:
    case SourceSpanKind::CommunicationObservation:
        return TemplateNodeRole::Annotation;
    }
    std::unreachable();
}

void TemplateDefinition::read_structure(const Json & source, const Json & source_graph, const Json & source_regions, const Json & calls,
                                        const std::map<SourceSpanId, SourceKernel> & kernels, std::vector<TemplateConnection> source_connections) {
    std::set<BoundaryId> referenced{ entry };
    for (const auto & identity : source.at("spans")) {
        const auto & span = source_graph.at("spans").at(identity.get<std::size_t>());
        referenced.insert(span.at("begin").get<BoundaryId>());
        referenced.insert(span.at("end").get<BoundaryId>());
    }

    for (const auto & identity : source.at("internal_edges")) {
        const auto & edge = source_graph.at("edges").at(identity.get<std::size_t>());
        referenced.insert(edge.at("source").get<BoundaryId>());
        referenced.insert(edge.at("target").get<BoundaryId>());
    }

    inputs = source.at("input_boundaries").get<std::vector<BoundaryId>>();
    outputs = source.at("output_boundaries").get<std::vector<BoundaryId>>();
    referenced.insert(inputs.begin(), inputs.end());
    referenced.insert(outputs.begin(), outputs.end());
    for (const auto & [port, boundary] : ports) referenced.insert(boundary);

    // Source identities remain provenance; all executable endpoints use local indices.
    std::map<BoundaryId, std::size_t> local_ids;
    std::map<std::size_t, std::size_t> lane_ids;
    for (const auto boundary : referenced) {
        local_ids.emplace(boundary, graph.boundary_count());
        const auto source_lane = source_graph.at("boundaries").at(boundary).at("lane").get<std::size_t>();
        const auto [position, inserted] = lane_ids.try_emplace(source_lane, lanes.size());
        if (inserted) {
            const auto & lane = source_graph.at("lanes").at(source_lane);
            lanes.push_back({
                source_lane,
                { SourceGraph::parse_domain(lane.at("domain").get<std::string>()),
                  lane.at("rank").get<std::uint32_t>(),
                  lane.at("native_id").get<std::uint64_t>() }
            });
        }
        graph.add_boundary({ position->second });
        boundary_observations.push_back({ boundary, source_graph.at("boundaries").at(boundary).at("observed_time_ns").get<std::int64_t>() });
    }

    std::map<SourceSpanId, std::size_t> node_ids;
    std::map<std::size_t, std::vector<std::size_t>> scoped_nodes;
    for (const auto & identity : source.at("spans")) {
        const auto source_span = identity.get<SourceSpanId>();
        const auto & span = source_graph.at("spans").at(source_span);
        auto scope = span.at("scope_call");
        while (!scope.is_null() && scope != source_call) {
            const auto identity = scope.get<std::size_t>();
            scoped_nodes[identity].push_back(graph.size());
            scope = calls.at(identity).at("parent");
        }
        node_ids.emplace(source_span, graph.size());
        const auto kernel = kernels.find(source_span);
        graph.push_back({ node_observations.size(),
                          span_kind(span.at("kind").get<std::string>()),
                          local_ids.at(span.at("begin").get<BoundaryId>()),
                          local_ids.at(span.at("end").get<BoundaryId>()) });
        node_observations.push_back({ source_span,
                                      kernel == kernels.end() ? nullptr : &kernel->second,
                                      span.at("scope_call").is_null() ? std::nullopt : std::optional(span.at("scope_call").get<std::size_t>()) });
    }

    for (const auto & identity : source.at("internal_edges")) {
        const auto source_edge = identity.get<std::size_t>();
        const auto & edge = source_graph.at("edges").at(source_edge);
        graph.add_dependency({ SourceGraph::parse_relation(edge.at("relation").get<std::string>()),
                               local_ids.at(edge.at("source").get<BoundaryId>()),
                               local_ids.at(edge.at("target").get<BoundaryId>()) });
    }

    // Keep native CPU paths across excluded observations inside the template,
    // so state qualification and later rewrites see the same dependencies.
    for (const auto & path : source.at("internal_thread_paths"))
        graph.add_dependency({ SourceRelation::ThreadOrder, local_ids.at(path.at(0).get<BoundaryId>()), local_ids.at(path.at(1).get<BoundaryId>()) });

    // A region is a view of existing work. Rebinding its cuts to the same local
    // boundary space keeps asynchronous dependencies without a region-wide barrier.
    for (const auto & source_region : source_regions) {
        if (source_region.at("serving_call").get<std::size_t>() != source_call) continue;
        TemplateRegion region{
            source_region.at("source_call").get<std::size_t>(), source_region.at("parent_call").get<std::size_t>(), std::nullopt, std::nullopt, {}, {}, {}
        };
        if (!source_region.at("binding").is_null()) region.binding = source_region.at("binding").get<std::string>();
        region.nodes = std::move(scoped_nodes[region.source_call]);
        for (const auto & boundary : source_region.at("input_boundaries")) region.inputs.push_back(local_ids.at(boundary.get<BoundaryId>()));
        for (const auto & boundary : source_region.at("output_boundaries")) region.outputs.push_back(local_ids.at(boundary.get<BoundaryId>()));
        regions.push_back(std::move(region));
    }

    // Operation windows refer to the same physical graph as region views.
    // Their presence records observed associations, not a closed rewrite rule.
    for (const auto & record : source_graph.at("collectives")) {
        const auto source_span = record.at("span").get<SourceSpanId>();
        if (source_graph.at("spans").at(source_span).at("owner_call") != source_call) continue;

        auto observation = SourceCollective::read(record);
        if (observation.rank != rank) throw std::invalid_argument("collective rank differs from its serving template");
        TemplateCollective collective{ .observation = std::move(observation),
                                       .envelope_node = node_ids.at(source_span),
                                       .task_nodes = {},
                                       .entry_boundaries = {},
                                       .exit_boundaries = {},
                                       .internal_dependencies = {} };
        for (const auto & task : collective.observation.tasks) collective.task_nodes.push_back(node_ids.at(task.span));
        collectives.push_back(std::move(collective));
    }

    // Bind physical work after collecting the observations. The collective
    // envelope remains an annotation and carries no additional service payload.
    for (const auto & collective : collectives) {
        for (const auto & task : collective.observation.tasks) observation(graph[node_ids.at(task.span)]).communication = &task;
    }

    for (const auto & operation : source_graph.at("operations")) {
        const auto host_id = operation.at("host_span").get<SourceSpanId>();
        const auto & host = source_graph.at("spans").at(host_id);
        if (host.at("owner_call") != source_call) continue;
        TemplateOperation view{
            operation.at("name").get<std::string>(),
            { host_id, local_ids.at(host.at("begin").get<BoundaryId>()), local_ids.at(host.at("end").get<BoundaryId>()) },
            {},
            operation.at("device_spans").get<std::vector<SourceSpanId>>()
        };
        for (const auto & identity : operation.at("worker_spans")) {
            const auto worker_id = identity.get<SourceSpanId>();
            const auto & worker = source_graph.at("spans").at(worker_id);
            view.workers.push_back({ worker_id, local_ids.at(worker.at("begin").get<BoundaryId>()), local_ids.at(worker.at("end").get<BoundaryId>()) });
        }
        if (!operation.at("input_shapes").is_null()) view.input_shapes = operation.at("input_shapes").get<std::string>();
        if (!operation.at("input_types").is_null()) view.input_types = operation.at("input_types").get<std::string>();
        view.collective_spans = operation.at("collective_spans").get<std::vector<SourceSpanId>>();
        operations.push_back(std::move(view));
    }

    for (const auto & observed : source_graph.at("stream_waits")) {
        const auto source_span = observed.at("source_span").get<SourceSpanId>();
        const auto & span = source_graph.at("spans").at(source_span);
        if (span.at("owner_call") != source_call) continue;

        TemplateStreamWait wait{ source_span, std::nullopt, local_ids.at(span.at("end").get<BoundaryId>()), {} };
        if (!observed.at("native_stream").is_null()) wait.native_stream = observed.at("native_stream").get<std::uint64_t>();
        const auto scope = observed.at("scope").get<std::string>();
        if (scope == "device") wait.scope = SynchronizationScope::Device;
        else if (scope != "stream") throw std::invalid_argument("unknown synchronization scope");
        for (const auto & identity : observed.at("lanes")) {
            const auto & lane = source_graph.at("lanes").at(identity.get<std::size_t>());
            wait.resources.emplace_back(SourceDomain::DeviceStream, lane.at("rank").get<std::uint32_t>(), lane.at("native_id").get<std::uint64_t>());
        }
        stream_waits.push_back(std::move(wait));
    }

    bind_collective_boundaries();

    // Ports and internal edges must refer to the same local boundary space.
    entry = local_ids.at(entry);
    for (auto & [port, boundary] : ports) boundary = local_ids.at(boundary);
    for (auto & boundary : inputs) boundary = local_ids.at(boundary);
    for (auto & boundary : outputs) boundary = local_ids.at(boundary);
    for (auto & connection : source_connections) {
        const auto & remote_point = source_graph.at("boundaries").at(connection.remote_source_boundary);
        const auto & remote_lane = source_graph.at("lanes").at(remote_point.at("lane").get<std::size_t>());
        graph.add_connection({ connection.relation, connection.direction, local_ids.at(connection.local_boundary) },
                             connection.remote_source_boundary,
                             { SourceGraph::parse_domain(remote_lane.at("domain").get<std::string>()),
                               remote_lane.at("rank").get<std::uint32_t>(),
                               remote_lane.at("native_id").get<std::uint64_t>() });
        connection_observations.push_back({ connection.source_edge, std::move(connection.remote_source_calls), connection.resource, connection.remote_order });
    }
}

TemplateConnection TemplateDefinition::connection(std::size_t index) const {
    const auto endpoint = graph.connection(index);
    const auto & contract = endpoint.contract;
    const auto & observed = connection_observations[index];
    return { observed.source_edge,         contract.relation, contract.direction,   contract.local_boundary, endpoint.remote_source_boundary,
             observed.remote_source_calls, observed.resource, observed.remote_order };
}

void TemplateDefinition::bind_collective_boundaries() {
    std::map<std::size_t, std::size_t> begin_owners;
    std::map<std::size_t, std::size_t> end_owners;
    std::vector<std::set<std::size_t>> entries(collectives.size());
    std::vector<std::set<std::size_t>> exits(collectives.size());
    std::vector<std::set<std::size_t>> ends(collectives.size());
    for (std::size_t index = 0; index < collectives.size(); ++index) {
        for (const auto node : collectives[index].task_nodes) {
            begin_owners.emplace(graph[node].begin, index);
            end_owners.emplace(graph[node].end, index);
            entries[index].insert(graph[node].begin);
            exits[index].insert(graph[node].end);
        }
        ends[index] = exits[index];
    }

    std::map<std::size_t, std::set<std::size_t>> joins;
    for (std::size_t index = 0; index < graph.dependencies().size(); ++index) {
        const auto & edge = graph.dependencies()[index];
        if (edge.relation == SourceRelation::CompletionJoin && lanes[graph.boundary(edge.target).lane].source.domain == SourceDomain::DeviceTaskCompletion)
            joins[edge.target].insert(edge.source);
        if (edge.relation != SourceRelation::NativeTaskDependency) continue;

        const auto source = end_owners.find(edge.source);
        const auto target = begin_owners.find(edge.target);
        if (source == end_owners.end() || target == begin_owners.end() || source->second != target->second) continue;
        const auto collective = source->second;
        collectives[collective].internal_dependencies.push_back(index);
        entries[collective].erase(edge.target);
        exits[collective].erase(edge.source);
    }

    // A collective completion is an existing physical-task join covering exactly
    // its members. The profiler envelope end is never a substitute for this gate.
    for (const auto & [boundary, inputs] : joins) {
        if (inputs.empty()) continue;
        const auto owner = end_owners.find(*inputs.begin());
        if (owner == end_owners.end() || inputs != ends[owner->second]) continue;
        auto & completion = collectives[owner->second].completion;
        if (completion) throw std::invalid_argument("collective has ambiguous physical task completion");
        completion = boundary;
    }
    for (std::size_t index = 0; index < collectives.size(); ++index) {
        collectives[index].entry_boundaries.assign(entries[index].begin(), entries[index].end());
        collectives[index].exit_boundaries.assign(exits[index].begin(), exits[index].end());
    }
}


std::optional<std::size_t> TemplateDefinition::logits_gather_reshape() const { return logits_reshape_operation; }

std::optional<std::size_t> TemplateDefinition::logits_view_service() const {
    if (source_requests.size() != 1) return std::nullopt;
    const auto index = logits_gather_reshape();
    if (!index) return std::nullopt;
    return operation_view_service(operations[*index]);
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

nlohmann::json TemplateSubgraphRegion::describe() const {
    return {
        {                  "local_nodes",                        nodes },
        {             "local_boundaries",                   boundaries },
        {               "crossing_nodes",               crossing_nodes },
        {        "retained_dependencies",        retained_dependencies },
        {        "internal_dependencies",        internal_dependencies },
        {        "incoming_dependencies",        incoming_dependencies },
        {        "outgoing_dependencies",        outgoing_dependencies },
        {         "external_connections",         external_connections },
        { "incoming_rotary_dependencies", incoming_rotary_dependencies },
        { "outgoing_rotary_dependencies", outgoing_rotary_dependencies }
    };
}

TemplateSubgraphRegion TemplateDefinition::operation_region(const TemplateOperation & operation) const {
    std::vector<TemplateOperationWindow> windows{ operation.host };
    windows.insert(windows.end(), operation.workers.begin(), operation.workers.end());

    std::set<SourceSpanId> observed_members(operation.device_spans.begin(), operation.device_spans.end());
    observed_members.insert(operation.collective_spans.begin(), operation.collective_spans.end());
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

TemplateSubgraphRegion TemplateDefinition::semantic_region(const TemplateRegion & source) const {
    TemplateSubgraphRegion region;
    region.nodes = source.nodes;
    std::set<std::size_t> points(source.inputs.begin(), source.inputs.end());
    points.insert(source.outputs.begin(), source.outputs.end());
    for (const auto index : region.nodes) {
        points.insert(graph[index].begin);
        points.insert(graph[index].end);
    }
    region.boundaries.assign(points.begin(), points.end());
    return complete_region(std::move(region));
}

TemplateSubgraphRegion TemplateDefinition::complete_region(TemplateSubgraphRegion region) const {
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

nlohmann::json TemplateRegionPort::describe() const {
    auto result = resource.describe();
    result["entry"] = entry;
    result["exit"] = exit;
    if (device_done) result["device_done"] = *device_done;
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

std::expected<std::vector<TemplateRegionPort>, std::string> TemplateDefinition::region_ports(const TemplateSubgraphRegion & region) const {
    if (!region.crossing_nodes.empty() || !region.incoming_rotary_dependencies.empty() || !region.outgoing_rotary_dependencies.empty())
        return std::unexpected("region_crossing_interface_not_supported");

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
                 || (connection.relation == SourceRelation::StreamOrder && connection.resource->domain == SourceDomain::DeviceStream)))
            return std::unexpected("region_external_relation_not_supported");

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
              || (edge.relation == SourceRelation::StreamOrder && lane.domain == SourceDomain::DeviceStream)))
            return std::unexpected("region_entry_relation_not_supported");
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
              || (edge.relation == SourceRelation::StreamOrder && lane.domain == SourceDomain::DeviceStream)))
            return std::unexpected("region_exit_relation_not_supported");
        exits[lane].insert(edge.source);
    }
    std::map<std::size_t, std::vector<std::size_t>> successors, predecessors;
    for (const auto index : region.internal_dependencies) {
        const auto & edge = graph.dependencies()[index];
        successors[edge.source].push_back(edge.target);
        predecessors[edge.target].push_back(edge.source);
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

        std::set<std::size_t> ancestors;
        std::vector<std::size_t> pending;
        for (const auto point : candidates) {
            const auto & previous = predecessors[point];
            pending.insert(pending.end(), previous.begin(), previous.end());
        }
        for (std::size_t cursor = 0; cursor < pending.size(); ++cursor) {
            const auto point = pending[cursor];
            if (!ancestors.insert(point).second) continue;
            const auto & previous = predecessors[point];
            pending.insert(pending.end(), previous.begin(), previous.end());
        }
        for (const auto point : ancestors) candidates.erase(point);
        if (candidates.size() != 1) return std::unexpected("region_terminal_work_not_unique");
        const auto exported = completion_exports.find(resource);
        if (exported != completion_exports.end() && exported->second != candidates) return std::unexpected("region_external_completion_not_terminal");
        if (resource.domain == SourceDomain::DeviceStream) completion_exports.emplace(resource, candidates);
        exits.emplace(resource, std::move(candidates));
    }
    for (const auto & [resource, points] : completion_exports) {
        if (!entries.contains(resource)) return std::unexpected("region_completion_resource_not_qualified");
    }
    if (entries.empty() || entries.size() != exits.size()) return std::unexpected("region_resource_interface_not_paired");

    std::vector<TemplateRegionPort> result;
    std::set<std::size_t> exposed;
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
                if (predecessors[exit] != std::vector<std::size_t>{ done }) return std::unexpected("region_completion_join_not_qualified");
                bool joined = false;
                for (const auto index : region.internal_dependencies) {
                    const auto & edge = graph.dependencies()[index];
                    if (edge.source == done && edge.target == exit && edge.relation == SourceRelation::CompletionJoin) joined = true;
                }
                if (!joined) return std::unexpected("region_completion_join_not_qualified");
            }
            result.back().device_done = done;
            exposed.insert(done);
        }
        exposed.insert(*incoming.begin());
        exposed.insert(*found->second.begin());
    }
    for (const auto index : region.retained_dependencies) {
        const auto & edge = graph.dependencies()[index];
        // An outside service may touch the region at just one endpoint. Only
        // its endpoints inside the cut need an explicit replacement mapping.
        if ((std::ranges::binary_search(region.boundaries, edge.source) && !exposed.contains(edge.source))
            || (std::ranges::binary_search(region.boundaries, edge.target) && !exposed.contains(edge.target)))
            return std::unexpected("region_retained_work_has_internal_endpoint");
    }
    for (const auto & [port, point] : ports)
        if (std::ranges::binary_search(region.boundaries, point)) return std::unexpected("region_contains_serving_port");

    for (const auto index : region.nodes) {
        const auto & node = graph[index];
        if (node.role() == TemplateNodeRole::Annotation) continue;
        const auto & lane = lanes[graph.boundary(node.begin).lane].resource();
        if (!entries.contains(lane)) return std::unexpected("region_work_has_no_resource_port");
    }

    for (const auto & port : result) {
        std::set<std::size_t> reached{ port.entry }, ancestors{ port.exit };
        std::vector<std::size_t> pending{ port.entry };
        for (std::size_t cursor = 0; cursor < pending.size(); ++cursor) {
            for (const auto point : successors[pending[cursor]])
                if (reached.insert(point).second) pending.push_back(point);
        }
        pending = { port.exit };
        for (std::size_t cursor = 0; cursor < pending.size(); ++cursor) {
            for (const auto point : predecessors[pending[cursor]])
                if (ancestors.insert(point).second) pending.push_back(point);
        }
        if (!reached.contains(port.exit)) return std::unexpected("region_resource_path_disconnected");

        for (const auto index : region.nodes) {
            const auto & node = graph[index];
            if (node.role() == TemplateNodeRole::Annotation) continue;
            const auto & lane = lanes[graph.boundary(node.begin).lane].resource();
            if (lane != port.resource) continue;
            if (!reached.contains(node.begin) || !ancestors.contains(node.end)) return std::unexpected("region_work_outside_resource_path");
        }
    }
    return result;
}

nlohmann::json TemplateDeviceOperationPorts::describe() const {
    return {
        { "stream_entry", stream_entry },
        {  "stream_exit",  stream_exit },
        {  "device_done",  device_done }
    };
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
    std::map<std::size_t, std::vector<std::size_t>> predecessors;
    std::set<std::size_t> terminal;
    for (const auto index : region.internal_dependencies) {
        const auto & edge = graph.dependencies()[index];
        predecessors[edge.target].push_back(edge.source);
        if (edge.relation == SourceRelation::CompletionJoin && edge.target == exit && device_ends.contains(edge.source)) terminal.insert(edge.source);
    }
    if (terminal.size() != 1) return std::unexpected("operation_device_completion_not_unique");
    const auto done = *terminal.begin();

    // Every recorded device completion must precede the exported final work.
    // This is a graph condition, never a comparison of observed timestamps.
    std::set<std::size_t> ancestors{ done };
    std::vector<std::size_t> pending{ done };
    for (std::size_t cursor = 0; cursor < pending.size(); ++cursor) {
        const auto found = predecessors.find(pending[cursor]);
        if (found == predecessors.end()) continue;
        for (const auto point : found->second) {
            if (ancestors.insert(point).second) pending.push_back(point);
        }
    }
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

nlohmann::json TemplateCollectiveOperationPorts::describe() const {
    return {
        {       "stream_predecessor", stream_predecessor ? Json(*stream_predecessor) : Json(nullptr) },
        {        "stream_completion",                                              stream_completion },
        {     "enclosing_completion",                                           enclosing_completion },
        { "notification_connections",                                       notification_connections }
    };
}

std::expected<TemplateCollectiveOperationPorts, std::string> TemplateDefinition::collective_operation_ports(const TemplateOperation & operation,
                                                                                                            const TemplateSubgraphRegion & region) const {
    if (operation.collective_spans.size() != 1 || !region.crossing_nodes.empty() || !region.incoming_rotary_dependencies.empty()
        || !region.outgoing_rotary_dependencies.empty())
        return std::unexpected("collective_operation_region_not_qualified");
    const auto found = std::ranges::find_if(collectives, [&operation](const TemplateCollective & collective) {
        return collective.observation.span == operation.collective_spans.front();
    });
    if (found == collectives.end() || !found->completion || found->entry_boundaries.size() != 1 || found->exit_boundaries.size() != 1)
        return std::unexpected("collective_operation_completion_not_qualified");

    std::set<SourceSpanId> members;
    std::set<std::size_t> begins, ends, predecessors, consumers;
    for (const auto index : found->task_nodes) {
        const auto & node = graph[index];
        members.insert(observation(node).source_span);
        begins.insert(node.begin);
        ends.insert(node.end);
    }
    if (members != std::set<SourceSpanId>(operation.device_spans.begin(), operation.device_spans.end()))
        return std::unexpected("collective_operation_members_not_qualified");
    std::set<std::size_t> cpu_entries{ operation.host.begin }, cpu_exits{ operation.host.end };
    for (const auto & worker : operation.workers) {
        cpu_entries.insert(worker.begin);
        cpu_exits.insert(worker.end);
    }

    for (const auto index : region.incoming_dependencies) {
        const auto & edge = graph.dependencies()[index];
        if (edge.relation == SourceRelation::ThreadOrder && cpu_entries.contains(edge.target)) continue;
        if (edge.relation == SourceRelation::StreamOrder && begins.contains(edge.target)) {
            predecessors.insert(edge.source);
            continue;
        }
        return std::unexpected("collective_operation_entry_not_qualified");
    }
    for (const auto index : region.outgoing_dependencies) {
        const auto & edge = graph.dependencies()[index];
        if (edge.relation == SourceRelation::ThreadOrder && cpu_exits.contains(edge.source)) continue;
        if (edge.relation == SourceRelation::StreamOrder && edge.source == *found->completion) continue;
        if (edge.relation == SourceRelation::CompletionJoin && ends.contains(edge.source)
            && lanes[graph.boundary(edge.target).lane].source.domain == SourceDomain::LogicalCompletion) {
            consumers.insert(edge.target);
            continue;
        }
        return std::unexpected("collective_operation_exit_not_qualified");
    }
    if (predecessors.size() > 1 || consumers.size() != 1) return std::unexpected("collective_operation_stream_interface_not_qualified");

    std::vector<std::size_t> notifications;
    for (const auto index : region.external_connections) {
        const auto & edge = this->connection(index);
        if (edge.relation != SourceRelation::NativeNotifySignal || (!begins.contains(edge.local_boundary) && !ends.contains(edge.local_boundary)))
            return std::unexpected("collective_operation_group_interface_not_qualified");
        notifications.push_back(index);
    }
    // This is a local interface qualification only. The group planner must
    // prove that every notification peer is in the same selected rank group.
    return TemplateCollectiveOperationPorts{ predecessors.empty() ? std::nullopt : std::optional(*predecessors.begin()),
                                             *found->completion,
                                             *consumers.begin(),
                                             std::move(notifications) };
}

std::expected<TemplateOperationRewrite, std::string> TemplateDefinition::plan_position_materialization_removal(const TemplateOperation & operation) const {
    const auto region = operation_region(operation);
    if (step != StepKind::Decode || operation.device_spans.size() != 1 || !region.crossing_nodes.empty())
        return std::unexpected("position_operation_boundary_not_supported");
    const auto layout = model_kernel_layouts.find(operation.device_spans.front());
    if (layout == model_kernel_layouts.end() || graph.model_rule(layout->second.node).role() != ModelKernelRole::PositionSlice)
        return std::unexpected("position_materialization_not_observed");
    if (!region.incoming_rotary_dependencies.empty() || region.outgoing_rotary_dependencies.empty())
        return std::unexpected("position_shared_consumers_not_qualified");

    const auto producer = rotary_position_dependencies[region.outgoing_rotary_dependencies.front()].producer;
    std::optional<std::size_t> tile;
    for (std::size_t index = 0; index < graph.size(); ++index) {
        const auto found = model_kernel_layouts.find(observation(graph[index]).source_span);
        if (found == model_kernel_layouts.end() || graph.model_rule(found->second.node).role() != ModelKernelRole::PositionTile
            || observation(graph[index]).source_scope != observation(graph[producer]).source_scope)
            continue;
        if (tile) return std::unexpected("position_view_producer_ambiguous");
        tile = index;
    }
    if (!tile) return std::unexpected("position_view_producer_missing");

    auto removal = plan_device_operation_removal(operation);
    if (!removal) return removal;
    removal->rotary_dependencies = region.outgoing_rotary_dependencies;
    removal->rotary_producer = *tile;
    return removal;
}

std::expected<TemplateOperationRewrite, std::string> TemplateDefinition::plan_device_operation_removal(const TemplateOperation & operation) const {
    const auto region = operation_region(operation);
    if (operation.device_spans.size() != 1 || !region.crossing_nodes.empty()) return std::unexpected("operation_boundary_not_supported");
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
    return removal;
}

bool TemplateDefinition::region_precedes(const TemplateRegion & first, const TemplateRegion & second) const {
    if (first.source_call == second.source_call || first.parent_call != second.parent_call) return false;
    if (std::ranges::any_of(first.nodes, [&](const auto index) { return std::ranges::binary_search(second.nodes, index); })) return false;
    const auto first_ports = region_ports(semantic_region(first));
    const auto second_ports = region_ports(semantic_region(second));
    if (!first_ports || !second_ports) return false;

    for (const auto & entry : *second_ports) {
        if ((entry.resource).domain != SourceDomain::CpuThread) continue;
        const auto previous = std::ranges::find(*first_ports, entry.resource, &TemplateRegionPort::resource);
        if (previous == first_ports->end()) continue;
        const auto lane = graph.boundary(entry.entry).lane;
        std::vector<bool> visited(graph.boundary_count(), false);
        std::vector<std::size_t> pending{ previous->exit };
        while (!pending.empty()) {
            const auto point = pending.back();
            pending.pop_back();
            if (visited[point]) continue;
            visited[point] = true;
            if (point == entry.entry) return true;
            for (const auto & edge : graph.dependencies()) {
                if (edge.source != point || (edge.relation != SourceRelation::ThreadOrder && edge.relation != SourceRelation::IntervalExtent)) continue;
                if (graph.boundary(edge.source).lane == lane && graph.boundary(edge.target).lane == lane) pending.push_back(edge.target);
            }
        }
    }
    return false;
}

std::expected<std::optional<TemplateRegionResourceAnchor>, std::string> TemplateDefinition::region_resource_anchor(const TemplateRegion & region,
                                                                                                                   const TemplateResource & resource) const {
    const auto recipient = semantic_region(region);
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
        const auto cut = semantic_region(candidate);
        if (!std::ranges::includes(cut.nodes, outside)) continue;
        if (std::ranges::any_of(cut.nodes, [&](const auto index) { return std::ranges::binary_search(recipient.nodes, index); })) continue;
        const auto ports = region_ports(cut);
        if (!ports) continue;
        const auto resource_port = std::ranges::find(*ports, resource, &TemplateRegionPort::resource);
        if (resource_port == ports->end()) continue;

        const bool before = region_precedes(candidate, region);
        const bool after = region_precedes(region, candidate);
        if (before == after) continue;
        if (anchor) return std::unexpected("region_resource_anchor_not_unique");
        anchor = TemplateRegionResourceAnchor{ *resource_port, static_cast<std::size_t>(&candidate - regions.data()), before };
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
    return plan_subgraph_replacement(semantic_region(region), donor, donor.semantic_region(donor_region), &region);
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
                                                                                                   const TemplateRegion * region) const {
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
    const auto mapping = result.insert_subgraph(donor, *this, donor_cut.nodes, donor_cut.internal_dependencies, std::move(points), lane_mapping);
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
                                    { { donor.graph.boundary(donor_operation.host.begin).lane, graph.boundary(operation.host.begin).lane } });

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

        replacement.insert_subgraph(donor, *this, donor_region.nodes, donor_region.internal_dependencies, std::move(boundary_mapping), lane_mapping);
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
        if (node.kind != SourceSpanKind::CpuObservation && node.kind != SourceSpanKind::CpuControl) return false;
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
