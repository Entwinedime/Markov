#include "markov/templates/templates.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace markov {

TemplateBoundary TemplateInstance::boundary(std::size_t index) const {
    if (index < definition->graph.boundary_count()) return definition->graph.boundary(index);
    return topology.inserted_boundaries().at(index - definition->graph.boundary_count()).boundary;
}

TemplateResource TemplateInstance::resource(TemplateResource source) const {
    if (source.rank == definition->rank) source.rank = rank;
    return source;
}

TemplateLane TemplateInstance::lane(std::size_t index) const {
    auto result = index < definition->lanes.size() ? definition->lanes[index] : topology.inserted_lanes().at(index - definition->lanes.size());
    result.source.rank = resource(result.resource()).rank;
    return result;
}

TemplateBoundaryIdentity TemplateInstance::boundary_identity(std::size_t index) const {
    const auto alias = topology.boundary_alias(index);
    if (alias != index) return TemplateEndpoint{ work_id, rank, alias };
    if (index < definition->graph.boundary_count()) return source_identity(definition->boundary_observations[index].source_boundary);
    return TemplateEndpoint{ work_id, rank, index };
}

TemplateSourceIdentity TemplateInstance::source_identity(std::size_t local) const {
    // Repeated calls retain source aliases within their target rank. Reusing
    // one physical shard for another rank creates a separate replica namespace.
    return { definition->source_manifest.native(), local, rank == definition->rank ? 0U : rank + 1 };
}

const SourceKernel * TemplateInstance::kernel(const TemplateNode & node) const {
    const auto & observed = definition->observation(node);
    const auto found = kernel_overrides.find(observed.source_span);
    return found == kernel_overrides.end() ? observed.kernel : &found->second;
}

const SourceTransfer * TemplateInstance::transfer(const TemplateNode & node) const {
    const auto & observed = definition->observation(node);
    const auto found = transfer_overrides.find(observed.source_span);
    return found == transfer_overrides.end() ? observed.transfer : &found->second;
}

TemplateInstance::TemplateInstance(WorkId work, const TemplateDefinition & source, std::uint32_t target_rank, const TemplateRankBinding & rank_binding)
    : work_id(work),
      rank(target_rank),
      rank_binding(&rank_binding),
      definition(&source),
      topology(source) {
    connections.reserve(source.graph.connection_count());
    for (std::size_t index = 0; index < source.graph.connection_count(); ++index) {
        auto connection = source.connection(index);
        if (connection.resource) connection.resource = resource(*connection.resource);
        connections.push_back({ std::move(connection), {} });
    }
}

std::expected<void, std::string> TemplateInstance::append_workspace_use(const TemplateDefinition & source, const TemplateNode & node,
                                                                        const BatchInput & input) {
    if (node.kind != SourceSpanKind::ResourceObservation) return {};
    const auto marker = source.observation(node).source_span;
    const auto found = std::ranges::find(source.workspace_uses, marker, &TemplateWorkspaceUse::marker);
    if (found == source.workspace_uses.end()) return {};

    std::optional<WorkspaceDemand> demand;
    if (found->demand) {
        auto evaluated = (*found->demand)->evaluate(input);
        if (!evaluated) return std::unexpected(evaluated.error());
        demand = *evaluated;
    }

    workspace_uses.push_back({ &source, static_cast<std::size_t>(found - source.workspace_uses.begin()), node.begin, node.end, demand });
    return {};
}

std::expected<void, std::string> TemplateInstance::bind_workspace_uses(const BatchInput & input) {
    workspace_uses.clear();
    // Traverse executable membership, not the original use list. Removed markers
    // disappear; each repeated donor marker keeps its own relocated endpoints.
    // Enumeration describes membership, not the execution order of state effects.
    for (const auto index : topology.nodes()) {
        auto bound = append_workspace_use(*definition, definition->graph[index], input);
        if (!bound) return bound;
    }
    for (const auto & inserted : topology.inserted_nodes()) {
        auto bound = append_workspace_use(*inserted.definition, inserted.node, input);
        if (!bound) return bound;
    }
    return {};
}

void TemplateInstance::collect_stream_waits() {
    // These imports are represented by surviving/replacement wait nodes below.
    // Keeping their source connections would retain edges to removed waits and
    // give unchanged waits two independent owners of the same dependency.
    std::erase_if(connections, [this](const auto & target) { return definition->is_resource_wait_import(target.connection); });

    std::set<SourceSpanId> surviving;
    for (const auto index : topology.nodes()) {
        const auto & node = definition->graph[index];
        if (node.kind == SourceSpanKind::HostWait) surviving.insert(definition->observation(node).source_span);
    }

    stream_waits.clear();
    for (const auto & wait : definition->stream_waits) {
        if (surviving.contains(wait.source_span)) stream_waits.push_back({ wait, {} });
    }

    // Repeated donor spans retain separate target completion boundaries.
    for (const auto & inserted : topology.inserted_nodes()) {
        if (inserted.stream_wait) stream_waits.push_back({ *inserted.stream_wait, {} });
    }
    for (auto & target : stream_waits)
        for (auto & source : target.wait.resources) source = resource(source);
}

void TemplateAssembly::prune_frontier(std::map<TemplateBoundaryIdentity, TemplateEndpoint> & candidates,
                                      const DependencyPaths<TemplateBoundaryIdentity> & paths) {
    std::set<TemplateBoundaryIdentity> boundaries;
    for (const auto & [boundary, endpoint] : candidates) boundaries.insert(boundary);
    const auto frontier = paths.frontier(std::move(boundaries));
    std::erase_if(candidates, [&frontier](const auto & candidate) { return !frontier.contains(candidate.first); });
}

std::vector<bool> TemplateInstance::reachable_boundaries(const std::vector<std::vector<std::size_t>> & adjacency, std::vector<std::size_t> pending) {
    std::vector<bool> reached(adjacency.size());
    while (!pending.empty()) {
        const auto point = pending.back();
        pending.pop_back();
        if (reached[point]) continue;
        reached[point] = true;
        pending.insert(pending.end(), adjacency[point].begin(), adjacency[point].end());
    }
    return reached;
}

std::expected<void, std::string> TemplateInstance::bind_inserted_waits() {
    if (std::ranges::none_of(topology.inserted_nodes(), [](const auto & node) { return node.stream_wait && node.stream_wait->has_resource_scope(); }))
        return {};

    const auto successors = topology.successors(*definition);
    std::vector<std::vector<std::size_t>> predecessors(successors.size());
    for (std::size_t point = 0; point < successors.size(); ++point)
        for (const auto next : successors[point]) predecessors[next].push_back(point);

    // Flow targets identify submitted device work, including device markers.
    // Keep actual target endpoints for both retained and inserted nodes.
    std::map<std::size_t, std::vector<std::size_t>> device_ends;
    for (const auto index : topology.nodes()) {
        const auto node = definition->graph[index];
        if (lane(boundary(node.begin).lane).source.domain == SourceDomain::DeviceStream) device_ends[node.begin].push_back(node.end);
    }
    for (const auto & inserted : topology.inserted_nodes()) {
        const auto & node = inserted.node;
        if (lane(boundary(node.begin).lane).source.domain == SourceDomain::DeviceStream) device_ends[node.begin].push_back(node.end);
    }
    std::vector<TemplateDependency> submissions;
    for (const auto index : topology.dependencies()) {
        const auto edge = definition->graph.dependencies()[index];
        if (edge.relation == SourceRelation::NativeFlow && device_ends.contains(edge.target)) submissions.push_back(edge);
    }
    for (const auto & edge : topology.added_dependencies())
        if (edge.relation == SourceRelation::NativeFlow && device_ends.contains(edge.target)) submissions.push_back(edge);

    for (const auto & inserted : topology.inserted_nodes()) {
        if (!inserted.stream_wait || !inserted.stream_wait->has_resource_scope()) continue;
        const auto & node = inserted.node;
        const auto worker = lane(boundary(node.begin).lane).resource();
        const auto before = reachable_boundaries(predecessors, { node.begin });
        const auto after = reachable_boundaries(successors, { node.end });
        const auto target = std::ranges::find_if(stream_waits, [&node](const auto & candidate) { return candidate.wait.completion == node.end; });

        for (const auto & observed_resource : inserted.stream_wait->resources) {
            const auto resource = this->resource(observed_resource);
            std::set<std::size_t> submitted;
            bool observed_worker = false;
            for (const auto & edge : submissions) {
                if (lane(boundary(edge.source).lane).resource() != worker || lane(boundary(edge.target).lane).resource() != resource) continue;
                observed_worker = true;
                if (before[edge.source]) submitted.insert(device_ends.at(edge.target).begin(), device_ends.at(edge.target).end());
                else if (!after[edge.source]) return std::unexpected("local_wait_submission_order_not_qualified");
            }
            if (!observed_worker) {
                for (const auto & [begin, ends] : device_ends)
                    if (lane(boundary(begin).lane).resource() == resource) return std::unexpected("local_wait_submission_worker_not_qualified");
            }

            // The worker may already have queued later operations. Only its
            // proven prior submissions participate, not control-thread aliases.
            std::vector<std::size_t> pending;
            for (const auto point : submitted) pending.insert(pending.end(), predecessors[point].begin(), predecessors[point].end());
            const auto ancestors = reachable_boundaries(predecessors, std::move(pending));
            for (const auto point : submitted)
                if (!ancestors[point]) target->binding.peers.push_back({ work_id, rank, point });
        }
    }
    return {};
}

std::vector<TemplateEndpoint> TemplateAssembly::frontier() const {
    std::vector<TemplateEndpoint> result;
    for (const auto & [resource, endpoints] : frontiers_) result.insert(result.end(), endpoints.begin(), endpoints.end());
    return result;
}

std::expected<void, std::string> TemplateAssembly::connect(std::span<TemplateInstance *> instances) {
    struct ObservedEndpoint {
        std::size_t source_call;
        TemplateEndpoint target;
    };
    // An imported wait is rebound once from its target resource query. Remove
    // both copies of its native cross-template edge, including removed waits.
    std::set<TemplateSourceIdentity> wait_imports;
    for (auto * instance : instances) {
        for (const auto & target : instance->connections) {
            if (instance->definition->is_resource_wait_import(target.connection)) wait_imports.insert(instance->source_identity(target.connection.source_edge));
        }
        instance->collect_stream_waits();
        const auto bound = instance->bind_inserted_waits();
        if (!bound) return bound;
    }
    for (auto * instance : instances) {
        std::erase_if(instance->connections, [&](const auto & target) {
            return target.connection.direction == TemplateConnectionDirection::Outgoing
                   && wait_imports.contains(instance->source_identity(target.connection.source_edge));
        });
    }

    std::map<TemplateSourceIdentity, std::vector<ObservedEndpoint>> endpoints;
    std::map<WorkId, std::size_t> invocation_order;
    std::map<std::pair<WorkId, std::uint32_t>, const TemplateInstance *> targets;
    for (const auto * instance : instances) targets.emplace(std::pair(instance->work_id, instance->rank), instance);
    std::map<TemplateSourceIdentity, std::map<std::int64_t, TemplateSourceIdentity>> cpu_order;
    for (const auto * instance : instances) {
        invocation_order.try_emplace(instance->work_id, invocation_order.size());
        const auto & definition = *instance->definition;
        for (std::size_t local = 0; local < definition.graph.boundary_count(); ++local) {
            const auto & boundary = definition.graph.boundary(local);
            const auto & observation = definition.boundary_observations[local];
            const auto identity = instance->source_identity(observation.source_boundary);
            endpoints[identity].push_back({
                definition.source_call,
                { instance->work_id, instance->rank, local }
            });
            const auto & lane = definition.lanes[boundary.lane];
            if (lane.source.domain == SourceDomain::CpuThread)
                cpu_order[instance->source_identity(lane.source_lane)].emplace(observation.observed_time_ns, identity);
        }
    }

    for (auto * instance : instances) {
        for (auto & target : instance->connections) {
            const auto & connection = target.connection;
            auto & binding = target.binding;
            // A joint rewrite has already bound inserted endpoints in target space.
            if (binding.rule != TemplateConnectionRule::Unresolved) continue;
            const auto found = endpoints.find(instance->source_identity(connection.remote_source_boundary));
            if (found != endpoints.end()) {
                // A boundary reference alone does not establish native ownership.
                for (const auto & endpoint : found->second) {
                    if (std::find(connection.remote_source_calls.begin(), connection.remote_source_calls.end(), endpoint.source_call)
                        != connection.remote_source_calls.end()) {
                        auto peer = endpoint.target;
                        const auto * owner = targets.at({ peer.work_id, peer.rank });
                        peer.boundary = owner->topology.boundary_alias(peer.boundary, connection.direction == TemplateConnectionDirection::Outgoing);
                        binding.peers.push_back(peer);
                    }
                }
            }
            if (!binding.peers.empty()) {
                binding.rule = TemplateConnectionRule::NativeBoundary;
                continue;
            }
            if (connection.relation != SourceRelation::ThreadOrder) continue;

            // SourceGraph orders every boundary on a native CPU thread. Project
            // that existing path through unmodeled observations without charging
            // their elapsed time or assigning their work to a template.
            const auto & definition = *instance->definition;
            const auto & boundary = definition.graph.boundary(connection.local_boundary);
            const auto & lane = definition.lanes[boundary.lane];
            const auto & order = cpu_order.at(instance->source_identity(lane.source_lane));
            auto neighbor = order.find(definition.boundary_observations[connection.local_boundary].observed_time_ns);
            if (connection.direction == TemplateConnectionDirection::Incoming) {
                if (neighbor == order.begin()) continue;
                --neighbor;
            }
            else {
                ++neighbor;
                if (neighbor == order.end()) continue;
            }

            for (const auto & endpoint : endpoints.at(neighbor->second)) {
                auto peer = endpoint.target;
                const auto * owner = targets.at({ peer.work_id, peer.rank });
                peer.boundary = owner->topology.boundary_alias(peer.boundary, connection.direction == TemplateConnectionDirection::Outgoing);
                binding.peers.push_back(peer);
            }
            binding.rule = TemplateConnectionRule::CpuThreadProjection;
        }
    }

    for (auto * instance : instances)
        for (auto & target : instance->connections)
            target.connection.local_boundary =
                instance->topology.boundary_alias(target.connection.local_boundary, target.connection.direction == TemplateConnectionDirection::Incoming);

    std::map<TemplateResource, std::map<TemplateBoundaryIdentity, TemplateEndpoint>> exports;
    for (auto * instance : instances) {
        const auto & definition = *instance->definition;
        for (auto & target : instance->connections) {
            const auto & connection = target.connection;
            auto & binding = target.binding;
            if (binding.rule != TemplateConnectionRule::Unresolved || !connection.resource) continue;
            const auto resource = *connection.resource;
            if (connection.direction == TemplateConnectionDirection::Outgoing) {
                const bool later = connection.remote_order && *connection.remote_order > definition.source_order;
                const bool thread_exit = connection.relation == SourceRelation::ThreadOrder && connection.remote_source_calls.empty();
                if ((later || thread_exit)
                    && (connection.relation == SourceRelation::ThreadOrder || connection.relation == SourceRelation::StreamOrder
                        || connection.relation == SourceRelation::NativeHostWait)) {
                    exports[resource].emplace(instance->boundary_identity(connection.local_boundary),
                                              TemplateEndpoint{ instance->work_id, instance->rank, connection.local_boundary });
                    binding.rule = TemplateConnectionRule::ResourceExport;
                }
                continue;
            }

            const bool earlier = connection.remote_order && *connection.remote_order < definition.source_order;
            const bool thread_entry = connection.relation == SourceRelation::ThreadOrder && connection.remote_source_calls.empty();
            if (!earlier && !thread_entry) continue;
            const auto previous = frontiers_.find(resource);
            if (previous == frontiers_.end()) binding.rule = TemplateConnectionRule::InitialResource;
            else if (!previous->second.empty()) {
                binding.peers = previous->second;
                binding.rule = TemplateConnectionRule::ResourcePredecessor;
            }
        }
    }

    // Every modeled service/wait exports its actual completion, including a
    // source capture's last batch. Explicit cross-batch edges alone omit that tail.
    DependencyPaths<TemplateBoundaryIdentity> predecessors;
    struct ResourceWork {
        TemplateResource resource;
        TemplateInstance * instance;
        std::size_t begin;
        std::size_t end;
    };
    std::vector<ResourceWork> resource_work;
    std::set<TemplateResource> introduced_resources;
    for (const auto * instance : instances) {
        for (const auto & port : instance->topology.added_resource_ports()) introduced_resources.insert(instance->resource(port.resource));
    }
    for (auto * instance : instances) {
        const auto & definition = *instance->definition;
        for (const auto index : instance->topology.nodes()) {
            const auto & node = definition.graph[index];
            if (node.role() == TemplateNodeRole::Annotation) continue;
            const auto & start_lane = instance->lane(instance->boundary(node.begin).lane).resource();
            if (introduced_resources.contains(start_lane)) resource_work.push_back({ start_lane, instance, node.begin, node.end });
            const auto & boundary = instance->boundary(node.end);
            const auto & lane = instance->lane(boundary.lane).resource();
            if (lane.domain != SourceDomain::CpuThread && lane.domain != SourceDomain::DeviceStream) continue;

            exports[lane].emplace(instance->boundary_identity(node.end), TemplateEndpoint{ instance->work_id, instance->rank, node.end });
        }
        for (const auto & inserted : instance->topology.inserted_nodes()) {
            const auto & node = inserted.node;
            if (node.role() == TemplateNodeRole::Annotation) continue;
            const auto & start_lane = instance->lane(instance->boundary(node.begin).lane).resource();
            if (introduced_resources.contains(start_lane)) resource_work.push_back({ start_lane, instance, node.begin, node.end });
            const auto & boundary = instance->boundary(node.end);
            const auto & lane = instance->lane(boundary.lane).resource();
            if (lane.domain != SourceDomain::CpuThread && lane.domain != SourceDomain::DeviceStream) continue;
            exports[lane].emplace(instance->boundary_identity(node.end), TemplateEndpoint{ instance->work_id, instance->rank, node.end });
        }
        for (const auto & port : instance->topology.collective_interfaces()) {
            const auto resource = instance->lane(instance->boundary(port.done).lane).resource();
            if (introduced_resources.contains(resource)) resource_work.push_back({ resource, instance, port.ready, port.done });
            exports[resource].emplace(instance->boundary_identity(port.done), TemplateEndpoint{ instance->work_id, instance->rank, port.done });
        }
        for (const auto index : instance->topology.dependencies()) {
            const auto & edge = definition.graph.dependencies()[index];
            predecessors.connect(instance->boundary_identity(edge.target), instance->boundary_identity(edge.source));
        }
        for (const auto & edge : instance->topology.added_dependencies()) {
            predecessors.connect(instance->boundary_identity(edge.target), instance->boundary_identity(edge.source));
        }
        for (const auto & target : instance->stream_waits)
            for (const auto & peer : target.binding.peers)
                predecessors.connect(instance->boundary_identity(target.wait.completion), instance->boundary_identity(peer.boundary));
        for (const auto & target : instance->connections) {
            const auto & binding = target.binding;
            if (binding.rule != TemplateConnectionRule::NativeBoundary && binding.rule != TemplateConnectionRule::CpuThreadProjection) continue;
            const auto & connection = target.connection;
            const auto local = instance->boundary_identity(connection.local_boundary);
            for (const auto & peer : binding.peers) {
                const auto remote = targets.at({ peer.work_id, peer.rank })->boundary_identity(peer.boundary);
                if (connection.direction == TemplateConnectionDirection::Incoming) predecessors.connect(local, remote);
                else predecessors.connect(remote, local);
            }
        }
    }

    // Only the earliest incomparable entries need a new resource dependency.
    // Existing paths carry it to later entries; no source-time ordering is used.
    std::set<TemplateEndpoint> resource_entries;
    if (!introduced_resources.empty()) {
        const auto successors = predecessors.reversed();
        std::map<std::pair<TemplateResource, WorkId>, std::map<TemplateBoundaryIdentity, TemplateEndpoint>> entries;
        for (const auto & work : resource_work) {
            entries[{ work.resource, work.instance->work_id }].emplace(work.instance->boundary_identity(work.begin),
                                                                       TemplateEndpoint{ work.instance->work_id, work.instance->rank, work.begin });
        }
        for (auto & [resource_work_id, candidates] : entries) {
            prune_frontier(candidates, successors);
            for (const auto & [boundary, endpoint] : candidates) resource_entries.insert(endpoint);
        }
    }

    // Added resource interfaces use target invocation order, never donor times.
    // A same-invocation neighbor is already connected by the region plan;
    // publish the inserted tail here for subsequent work and waits.
    // This path is the qualified non-overlap cohort: submissions on the same
    // worker/stream remain FIFO across its sequential serving actions.
    for (auto * instance : instances) {
        for (auto port : instance->topology.added_resource_ports()) {
            port.resource = instance->resource(port.resource);
            port.entry = instance->topology.boundary_alias(port.entry, true);
            port.exit = instance->topology.boundary_alias(port.exit);
            auto & binding = instance->resource_imports[port.entry];
            const auto previous = frontiers_.find(port.resource);
            binding.rule = TemplateConnectionRule::InitialResource;
            if (previous != frontiers_.end()) {
                if (previous->second.empty()) binding.rule = TemplateConnectionRule::Unresolved;
                else {
                    binding.rule = TemplateConnectionRule::ResourcePredecessor;
                    binding.peers = previous->second;
                }
            }
            for (const auto & work : resource_work) {
                if (work.resource != port.resource || work.instance->work_id == instance->work_id) continue;
                if (invocation_order.at(work.instance->work_id) < invocation_order.at(instance->work_id)) {
                    binding.peers.push_back({ work.instance->work_id, work.instance->rank, work.end });
                    if (binding.rule != TemplateConnectionRule::Unresolved) binding.rule = TemplateConnectionRule::ResourcePredecessor;
                }
                else {
                    if (!resource_entries.contains({ work.instance->work_id, work.instance->rank, work.begin })) continue;
                    work.instance->resource_dependencies.push_back({
                        { instance->work_id, instance->rank, port.exit },
                        work.begin
                    });
                    predecessors.connect(work.instance->boundary_identity(work.begin), instance->boundary_identity(port.exit));
                }
            }
            for (const auto & peer : binding.peers) {
                const auto local = targets.find({ peer.work_id, peer.rank });
                if (local != targets.end()) predecessors.connect(instance->boundary_identity(port.entry), local->second->boundary_identity(peer.boundary));
            }
            exports[port.resource].emplace(instance->boundary_identity(port.exit), TemplateEndpoint{ instance->work_id, instance->rank, port.exit });
        }
    }

    // A non-overlap serving action has submitted all of its work before the
    // next action can issue a stream wait. Import preceding actions, including
    // those in this batch, but never the waiting action's own future exports.
    // Dependencies inside that action remain owned by its region rewrites.
    for (auto * instance : instances) {
        std::map<TemplateResource, std::map<TemplateBoundaryIdentity, TemplateEndpoint>> prior;
        for (const auto & target : instance->stream_waits) {
            if (!target.wait.has_resource_scope()) continue;
            for (const auto & resource : target.wait.resources) prior.try_emplace(resource);
        }
        for (auto & [resource, candidates] : prior) {
            const auto submitted = exports.find(resource);
            if (submitted == exports.end()) continue;
            for (const auto & [boundary, endpoint] : submitted->second) {
                if (invocation_order.at(endpoint.work_id) < invocation_order.at(instance->work_id)) candidates.emplace(boundary, endpoint);
            }
            prune_frontier(candidates, predecessors);
        }
        for (auto & target : instance->stream_waits) {
            const auto & wait = target.wait;
            auto & binding = target.binding;
            if (!wait.has_resource_scope()) continue;

            binding.rule = TemplateConnectionRule::InitialResource;
            for (const auto & resource : wait.resources) {
                const auto previous = frontiers_.find(resource);
                if (previous != frontiers_.end()) {
                    if (previous->second.empty()) {
                        binding.rule = TemplateConnectionRule::Unresolved;
                        break;
                    }
                    binding.peers.insert(binding.peers.end(), previous->second.begin(), previous->second.end());
                }
                for (const auto & [boundary, endpoint] : prior.at(resource)) {
                    binding.peers.push_back(endpoint);
                    predecessors.connect(instance->boundary_identity(wait.completion), boundary);
                }
            }
            if (binding.rule != TemplateConnectionRule::Unresolved && !binding.peers.empty()) binding.rule = TemplateConnectionRule::ResourcePredecessor;
        }
    }

    // Prune only with proven paths; incomparable resource ends survive.
    for (auto & [resource, candidates] : exports) prune_frontier(candidates, predecessors);

    // Source lanes are provenance, not target activity. A resource with no
    // surviving work or export retains its previous submitted frontier.
    for (const auto & [resource, candidates] : exports) {
        auto & frontier = frontiers_[resource];
        frontier.clear();
        // Several incomparable exports are not silently collapsed by source time.
        for (const auto & [source, endpoint] : candidates) frontier.push_back(endpoint);
    }
    return {};
}

} // namespace markov
