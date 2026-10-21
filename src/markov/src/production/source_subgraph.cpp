#include "markov/production/source_subgraph.hpp"

#include "markov/templates/templates.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace markov {

TemplateDefinition BlockSubgraph::build(const SourceGraph & source_graph, std::span<const SourceCall> calls, std::span<const RegionSubgraph> source_regions,
                                        const std::map<SourceSpanId, SourceKernel> & kernels, std::vector<TemplateConnection> source_connections) const {
    const auto & observed = calls[call];
    TemplateDefinition result{ observed.step, observed.phase.value_or(BatchPhase::Idle), observed.rank, call };
    result.entry = source_graph.spans().at(observed.marker.value()).begin;
    result.source_batch = observed.batch_id;
    result.source_requests = observed.requests;
    result.source_incoming_requests = observed.incoming_requests;
    if (observed.input_supported == false) result.input_reception = false;
    else if (observed.input_supported == true) result.input_reception = std::unexpected("native_input_frontier_not_observed");
    for (const auto port : ServingContract{}.block(observed.step).exits) result.ports.emplace(port, ports.at(port));

    std::set<BoundaryId> referenced{ result.entry };
    referenced.insert(logical_boundaries.begin(), logical_boundaries.end());
    const auto input_endpoint = [&](std::size_t identity) -> std::optional<BoundaryId> {
        const auto & scope = calls[identity];
        if (scope.serving_parent(calls) != call || !scope.marker) return std::nullopt;
        return source_graph.spans()[*scope.marker].end;
    };
    for (const auto & [id, scope] : observed.request_ready_calls) {
        const auto request = std::ranges::find(result.source_incoming_requests, id, &SourceIncomingRequest::id);
        const auto endpoint = input_endpoint(scope);
        if (request == result.source_incoming_requests.end() || !endpoint) continue;
        result.incoming_request_ready.emplace_back(static_cast<std::size_t>(request - result.source_incoming_requests.begin()), *endpoint);
        referenced.insert(*endpoint);
    }

    for (const auto & identity : spans) {
        const auto & span = source_graph.spans().at(identity);
        referenced.insert(span.begin);
        referenced.insert(span.end);
    }

    for (const auto & identity : edges) {
        const auto & edge = source_graph.edges().at(identity);
        referenced.insert(edge.source);
        referenced.insert(edge.target);
    }

    // Cross-block cuts are owned by the actual connections, not a second inventory.
    for (const auto & connection : source_connections) referenced.insert(connection.local_boundary);
    for (const auto & [port, boundary] : result.ports) referenced.insert(boundary);

    // Source identities remain provenance; all executable endpoints use local indices.
    std::map<BoundaryId, std::size_t> local_ids;
    std::map<std::size_t, std::size_t> lane_ids;
    for (const auto boundary : referenced) {
        local_ids.emplace(boundary, result.graph.boundary_count());
        const auto source_lane = source_graph.boundaries().at(boundary).lane;
        const auto [position, inserted] = lane_ids.try_emplace(source_lane, result.lanes.size());
        if (inserted) {
            const auto & lane = source_graph.lanes().at(source_lane);
            result.lanes.push_back({ source_lane, lane });
        }
        result.graph.add_boundary({ position->second });
        result.boundary_observations.push_back({ boundary, source_graph.boundaries().at(boundary).observed_time_ns });
        const auto cpu = source_graph.cpu_boundaries().find(boundary);
        if (cpu != source_graph.cpu_boundaries().end()) result.cpu_boundaries.emplace(local_ids.at(boundary), cpu->second);
    }

    std::map<std::size_t, std::vector<std::size_t>> scoped_nodes;
    for (const auto source_span : spans) {
        const auto & span = source_graph.spans().at(source_span);
        auto scope = span.scope_call;
        while (scope && *scope != result.source_call) {
            scoped_nodes[*scope].push_back(result.graph.size());
            scope = calls[*scope].parent;
        }
        const auto kernel = kernels.find(source_span);
        result.graph.push_back({ result.node_observations.size(), span.kind, local_ids.at(span.begin), local_ids.at(span.end) });
        result.node_observations.push_back({ source_span, kernel == kernels.end() ? nullptr : &kernel->second, span.scope_call });
    }

    for (const auto identity : edges) {
        const auto & edge = source_graph.edges().at(identity);
        result.graph.add_dependency({ edge.relation, local_ids.at(edge.source), local_ids.at(edge.target) });
    }

    // Keep native CPU paths across excluded observations inside the template,
    // so state qualification and later rewrites see the same dependencies.
    for (const auto & path : thread_paths) result.graph.add_dependency({ SourceRelation::ThreadOrder, local_ids.at(path[0]), local_ids.at(path[1]) });

    // A region is a view of existing work. Rebinding its cuts to the same local
    // boundary space keeps asynchronous dependencies without a region-wide barrier.
    for (const auto & source_region : source_regions) {
        if (source_region.serving_call != result.source_call) continue;
        const auto & region_call = calls[source_region.call];
        TemplateRegion region(source_region.call, *region_call.parent);
        if (region_call.marker) {
            const auto & marker = source_graph.spans()[*region_call.marker];
            region.cpu_scope = TemplateRegionScope{ local_ids.at(marker.begin), local_ids.at(marker.end) };
        }
        region.nodes = std::move(scoped_nodes[region.source_call]);
        for (const auto & boundary : source_region.inputs) region.inputs.push_back(local_ids.at(boundary));
        for (const auto & boundary : source_region.outputs) region.outputs.push_back(local_ids.at(boundary));
        result.regions.push_back(std::move(region));
    }

    for (const auto & [host_id, operation] : source_graph.operations()) {
        const auto & host = source_graph.spans().at(host_id);
        if (host.owner_call != result.source_call) continue;
        TemplateOperation view{
            operation.name,
            { host_id, local_ids.at(host.begin), local_ids.at(host.end) },
            {},
            operation.device_spans
        };
        for (const auto worker_id : operation.worker_spans) {
            const auto & worker = source_graph.spans().at(worker_id);
            view.workers.push_back({ worker_id, local_ids.at(worker.begin), local_ids.at(worker.end) });
        }
        result.operations.push_back(std::move(view));
    }

    for (const auto & [source_span, observed] : source_graph.stream_waits()) {
        const auto & span = source_graph.spans().at(source_span);
        if (span.owner_call != result.source_call) continue;

        TemplateStreamWait wait{ source_span, observed.native_stream, local_ids.at(span.end), {}, observed.scope };
        for (const auto identity : observed.lanes) {
            const auto & lane = source_graph.lanes().at(identity);
            wait.resources.emplace_back(SourceDomain::DeviceStream, lane.rank, lane.native_id);
        }
        result.stream_waits.push_back(std::move(wait));
    }

    // Ports and internal edges must refer to the same local boundary space.
    result.entry = local_ids.at(result.entry);
    for (auto & [slot, boundary] : result.incoming_request_ready) boundary = local_ids.at(boundary);
    for (auto & [port, boundary] : result.ports) boundary = local_ids.at(boundary);
    for (auto & connection : source_connections) {
        result.graph.add_connection({ connection.relation, connection.direction, local_ids.at(connection.local_boundary) }, connection.remote_source_boundary);
        result.connection_observations.push_back(
            { connection.source_edge, std::move(connection.remote_source_calls), connection.resource, connection.remote_order });
    }
    return result;
}

} // namespace markov
