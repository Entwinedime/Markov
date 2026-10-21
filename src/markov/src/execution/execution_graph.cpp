#include "markov/execution/execution_graph.hpp"

#include "markov/graph/directed_graph.hpp"
#include "markov/performance/cost_query.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <utility>

namespace markov {

namespace {

std::string_view role_name(TemplateNodeRole role) {
    switch (role) {
    case TemplateNodeRole::Service:
        return "service";
    case TemplateNodeRole::Wait:
        return "wait";
    case TemplateNodeRole::Annotation:
        return "annotation";
    }
    std::unreachable();
}

nlohmann::json logical_scope(const TemplateDefinition & definition, std::optional<std::size_t> call, std::string_view binding,
                             std::optional<std::size_t> request_slot = std::nullopt, std::optional<std::uint32_t> model_layer = std::nullopt,
                             const std::vector<std::string_view> * mapped_path = nullptr, std::optional<std::size_t> input_slot = std::nullopt) {
    const auto scope = definition.semantic_scope(call);
    if (scope) {
        if (!request_slot) request_slot = scope->request_slot;
        if (!model_layer) model_layer = scope->model_layer;
        if (!input_slot) input_slot = scope->input_slot;
    }
    const auto * path = mapped_path ? mapped_path : scope ? &scope->path : nullptr;
    return {
        {      "binding",                                                                binding },
        {         "path",                 path ? nlohmann::json(*path) : nlohmann::json(nullptr) },
        { "request_slot", request_slot ? nlohmann::json(*request_slot) : nlohmann::json(nullptr) },
        {  "model_layer",   model_layer ? nlohmann::json(*model_layer) : nlohmann::json(nullptr) },
        {   "input_slot",     input_slot ? nlohmann::json(*input_slot) : nlohmann::json(nullptr) }
    };
}

nlohmann::json collective_scope(const TemplateDefinition & definition, const TemplateCollectiveInstance * interface, std::string_view role) {
    std::optional<std::size_t> call;
    if (interface) call = interface->recipient_region ? definition.regions[*interface->recipient_region].source_call : definition.source_call;

    auto result = logical_scope(definition, call, "recipient_owner", std::nullopt, interface ? interface->model_layer : std::nullopt);
    result["collective_role"] = role;
    result["collective_position"] =
        interface ? nlohmann::json(interface->definition->collective_interfaces[interface->interface].position) : nlohmann::json(nullptr);
    return result;
}

} // namespace

ExecutionGraph::ExecutionGraph(std::optional<std::filesystem::path> path) : path_(std::move(path)) {
    if (!path_) return;

    std::filesystem::create_directories(path_->parent_path());
    auto temporary = *path_;
    temporary += ".tmp";
    diagnostics_.exceptions(std::ios::failbit | std::ios::badbit);
    diagnostics_.open(temporary);
}

BoundaryId ExecutionGraph::resolve(const TemplateEndpoint & endpoint) {
    const auto local = instance_boundaries_.find({ endpoint.work_id, endpoint.rank });
    if (local != instance_boundaries_.end()) return local->second.at(endpoint.boundary);

    const auto [found, inserted] = resource_imports_.try_emplace(endpoint, boundaries_.size());
    if (inserted) boundaries_.push_back({ endpoint, resource_times_.at(endpoint) });
    return found->second;
}

BoundaryId ExecutionGraph::resolve(const Gate & gate) {
    const auto local = gates_.find(gate);
    if (local != gates_.end()) return local->second;

    const auto [found, inserted] = gate_imports_.try_emplace(gate, boundaries_.size());
    if (inserted) boundaries_.push_back({ gate, port_times_.at(gate) });
    return found->second;
}

void ExecutionGraph::retain(std::span<const TemplateEndpoint> resources, std::span<const PortRef> ports) {
    std::map<TemplateEndpoint, std::int64_t> resource_times;
    for (const auto & endpoint : resources) {
        const auto local = instance_boundaries_.find({ endpoint.work_id, endpoint.rank });
        const auto time = local == instance_boundaries_.end() ? resource_times_.at(endpoint) : completion_ns_.at(local->second.at(endpoint.boundary));
        resource_times.emplace(endpoint, time);
    }
    resource_times_ = std::move(resource_times);

    std::map<Gate, std::int64_t> port_times;
    for (const auto & port : ports) {
        for (const auto & [gate, time] : port_times_)
            if (gate.work == port.work_id && gate.kind == port.port) port_times.emplace(gate, time);
        for (const auto & [gate, point] : gates_)
            if (gate.work == port.work_id && gate.kind == port.port) port_times.emplace(gate, completion_ns_.at(point));
    }
    port_times_ = std::move(port_times);
}

void ExecutionGraph::release_batch() {
    boundaries_.clear();
    nodes_.clear();
    logical_scopes_.clear();
    edges_.clear();
    ports_.clear();
    outputs_.clear();
    inputs_.clear();
    assembly_gap_.reset();
    unresolved_.clear();
    instance_boundaries_.clear();
    gates_.clear();
    resource_imports_.clear();
    gate_imports_.clear();
    completion_ns_.clear();
}

std::size_t ExecutionGraph::register_source(const std::filesystem::path & manifest) {
    const auto found = std::ranges::find(sources_, manifest);
    if (found != sources_.end()) return static_cast<std::size_t>(found - sources_.begin());
    sources_.push_back(manifest);
    const auto index = sources_.size() - 1;
    if (diagnostics_.is_open())
        diagnostics_ << nlohmann::json({
            {       "record",          "source" },
            { "source_asset",             index },
            {     "manifest", manifest.string() }
        })
                     << '\n';
    return index;
}

bool ExecutionGraph::append(std::span<TemplateInstance *> instances, std::span<const InvokeStep> invocations, std::int64_t release_ns) {
    std::map<std::tuple<std::size_t, BoundaryId, std::uint32_t>, BoundaryId> shared;
    for (const auto * instance : instances) {
        const auto & definition = *instance->definition;
        const auto source_asset = register_source(definition.source_manifest);
        auto & local = instance_boundaries_[{ instance->work_id, instance->rank }];
        local.reserve(definition.graph.boundary_count());
        for (std::size_t index = 0; index < definition.boundary_observations.size(); ++index) {
            const auto & boundary = definition.boundary_observations[index];
            const auto [position, inserted] =
                shared.try_emplace(std::tuple{ source_asset, boundary.source_boundary, instance->source_identity(0).replica }, boundaries_.size());
            if (inserted)
                boundaries_.push_back({
                    ObservedBoundary{ boundary.source_boundary, source_asset },
                    release_ns,
                    definition.cpu_boundary(index)
                });
            local.push_back(position->second);
        }
        // Donor provenance does not grant sharing with any other insertion.
        for (const auto & boundary : instance->topology.inserted_boundaries()) {
            local.push_back(boundaries_.size());
            boundaries_.push_back({
                ObservedBoundary{ boundary.source_boundary, register_source(*boundary.source_manifest) },
                release_ns,
                boundary.cpu
            });
        }
    }

    // Allocate shared target identities before applying a replacement's actual
    // CPU observation. Donor provenance must not merge independent endpoints.
    std::map<BoundaryId, const TemplateInsertedBoundary *> rebound;
    for (const auto * instance : instances) {
        const auto & local = instance_boundaries_.at({ instance->work_id, instance->rank });
        for (const auto & [point, observation] : instance->topology.rebound_boundaries()) {
            const auto target = local[point];
            const auto [previous, inserted] = rebound.try_emplace(target, &observation);
            if (!inserted) {
                if (!previous->second->same_cpu_boundary(observation)) throw std::logic_error("shared target CPU boundary observations disagree");
                continue;
            }
            auto & boundary = boundaries_[target];
            boundary.origin = ObservedBoundary{ observation.source_boundary, register_source(*observation.source_manifest) };
            boundary.cpu = observation.cpu;
        }
    }

    // A source point is shared within this batch only. Reusing the same template
    // in a later iteration creates new target points instead of aliasing old work.
    std::set<std::tuple<std::size_t, SourceSpanId, std::uint32_t>> members;
    std::set<std::tuple<BoundaryId, BoundaryId, std::optional<SourceRelation>>> constraints;
    // Resolved connections retain their edge semantics. Only interfaces with
    // no current edge need an explicit cut for CPU service grouping.
    std::vector<BoundaryId> interfaces;
    std::map<std::pair<WorkId, std::uint32_t>, std::set<BoundaryId>> device_work;
    for (const auto * instance : instances) {
        const auto & definition = *instance->definition;
        const auto source_asset = register_source(definition.source_manifest);
        const auto & local = instance_boundaries_.at({ instance->work_id, instance->rank });
        using CollectiveKey = std::pair<std::string_view, std::optional<std::uint32_t>>;
        std::map<CollectiveKey, const CollectiveWork *> collective_work;
        for (const auto & port : instance->topology.collective_interfaces()) {
            const auto & source = port.definition->collective_interfaces[port.interface];
            const auto [found, inserted] = collective_work.emplace(CollectiveKey{ source.position, port.model_layer }, port.work);
            if (!inserted && found->second != port.work) found->second = nullptr;
        }
        struct CpuOwner {
            const TemplateCollectiveInterface * collective = nullptr;
            std::optional<std::size_t> input_slot;
        };
        std::map<std::pair<const TemplateDefinition *, std::optional<std::size_t>>, CpuOwner> cpu_owners;
        const auto bind_cpu_work = [&](Node & node,
                                       const TemplateDefinition & source,
                                       std::optional<std::size_t>
                                           scope,
                                       std::optional<std::uint32_t> layer = std::nullopt,
                                       const TemplateSemanticPath * mapped_path = nullptr,
                                       std::optional<std::size_t> input_slot = std::nullopt) {
            if (node.kind != SourceSpanKind::CpuControl) return;
            const auto [cached, fresh] = cpu_owners.try_emplace({ &source, scope });
            if (fresh) {
                auto ancestor = scope;
                while (ancestor && *ancestor != source.source_call) {
                    const auto region = std::ranges::find(source.regions, *ancestor, &TemplateRegion::source_call);
                    if (region == source.regions.end()) break;
                    if (!cached->second.input_slot && region->input_slot) cached->second.input_slot = region->input_slot;
                    if (!cached->second.collective) {
                        const auto owner = std::ranges::find_if(source.collective_interfaces, [&](const auto & candidate) {
                            return source.regions[candidate.owner_region].source_call == *ancestor;
                        });
                        if (owner != source.collective_interfaces.end()) cached->second.collective = &*owner;
                    }
                    ancestor = region->parent_call;
                }
            }
            const auto fail = [&](std::string reason) {
                if (!assembly_gap_) assembly_gap_ = ExecutionTimingGap{ std::move(reason), node.work, node.rank, node.source, source.source_manifest };
            };
            const auto & owner = cached->second;
            if (owner.input_slot) {
                if (mapped_path && (!*mapped_path || !input_slot)) {
                    fail("cpu_input_handler_target_scope_not_bound");
                    return;
                }
                node.input_slot = mapped_path ? input_slot : owner.input_slot;
            }
            if (!owner.collective) return;
            if (mapped_path) {
                if (!*mapped_path) {
                    fail("cpu_collective_target_scope_not_bound");
                    return;
                }
                const auto relative = source.semantic_scope(scope, source.regions[owner.collective->owner_region].source_call);
                if (!relative || (*mapped_path)->size() < relative->path.size()
                    || !std::equal(relative->path.rbegin(), relative->path.rend(), (*mapped_path)->rbegin())) {
                    fail("cpu_collective_scope_rebase_not_bound");
                    return;
                }
            }
            if (!layer) layer = owner.collective->model_layer;
            const auto work = collective_work.find({ owner.collective->position, layer });
            if (work == collective_work.end() || !work->second) fail("cpu_collective_work_not_uniquely_bound");
            else node.collective = work->second;
        };
        for (const auto index : instance->topology.nodes()) {
            const auto & node = definition.graph[index];
            const auto & node_observation = definition.observation(node);
            if (members.emplace(source_asset, node_observation.source_span, instance->source_identity(0).replica).second) {
                const auto * transfer = instance->transfer(node);
                nodes_.push_back({ instance->work_id,
                                   instance->rank,
                                   source_asset,
                                   node_observation.source_span,
                                   node.role(),
                                   node.kind,
                                   instance->kernel(node),
                                   local[node.begin],
                                   local[node.end],
                                   transfer,
                                   node_observation.device_command ? &*node_observation.device_command : nullptr });
                nodes_.back().workspace = workspace_effect(*instance, node);
                bind_cpu_work(nodes_.back(), definition, node_observation.source_scope);
                if (instance->lane(instance->boundary(node.begin).lane).resource().domain == SourceDomain::DeviceStream)
                    device_work[{ instance->work_id, instance->rank }].insert(local[node.end]);
                if (diagnostics_.is_open()) {
                    // Cut membership already proves the serving owner. No inner
                    // scope means its root; an unresolved explicit scope stays unknown.
                    const auto scope = node.kind == SourceSpanKind::CpuControl && !node_observation.source_scope ? std::optional(definition.source_call)
                                                                                                                 : node_observation.source_scope;
                    logical_scopes_.push_back(logical_scope(definition, scope, "source_scope"));
                    if (node.kind == SourceSpanKind::CpuControl) nodes_.back().cpu_resource = instance->lane(instance->boundary(node.begin).lane).resource();
                }
            }
        }

        // Inserted work has target-local endpoints. Its source span is cost
        // provenance, not an identity to deduplicate against other instances.
        for (const auto & inserted : instance->topology.inserted_nodes()) {
            const auto & node = inserted.node;
            const auto * transfer = inserted.transfer();
            nodes_.push_back({ instance->work_id,
                               instance->rank,
                               register_source(inserted.definition->source_manifest),
                               inserted.observation().source_span,
                               node.role(),
                               node.kind,
                               inserted.kernel(),
                               local[node.begin],
                               local[node.end],
                               transfer,
                               inserted.observation().device_command ? &*inserted.observation().device_command : nullptr });
            nodes_.back().workspace = workspace_effect(*instance, node);
            bind_cpu_work(nodes_.back(),
                          *inserted.definition,
                          inserted.observation().source_scope,
                          inserted.model_layer,
                          &inserted.semantic_path,
                          inserted.input_slot);
            if (instance->lane(instance->boundary(node.begin).lane).resource().domain == SourceDomain::DeviceStream)
                device_work[{ instance->work_id, instance->rank }].insert(local[node.end]);
            if (diagnostics_.is_open()) {
                if (node.kind == SourceSpanKind::CpuControl) nodes_.back().cpu_resource = instance->lane(instance->boundary(node.begin).lane).resource();
                // Provenance stays with the donor. Only a qualified cut mapping
                // supplies its relative semantics in the complete target path.
                const auto call = inserted.recipient_region ? definition.regions[*inserted.recipient_region].source_call : definition.source_call;
                logical_scopes_.push_back(logical_scope(definition,
                                                        call,
                                                        inserted.semantic_path ? "target_scope" : "recipient_owner",
                                                        inserted.request_slot,
                                                        inserted.model_layer,
                                                        inserted.semantic_path.get(),
                                                        inserted.input_slot));
            }
        }

        for (const auto index : instance->topology.dependencies()) {
            const auto & edge = definition.graph.dependencies()[index];
            constraints.emplace(local[edge.source], local[edge.target], edge.relation);
        }
        for (const auto & edge : instance->topology.added_dependencies()) constraints.emplace(local[edge.source], local[edge.target], edge.relation);
        for (const auto & dependency : instance->topology.rotary_dependencies()) {
            const auto & producer = definition.graph[dependency.producer];
            const auto & consumer = definition.graph[dependency.consumer];
            constraints.emplace(local[producer.end], local[consumer.begin], SourceRelation::ModelDataDependency);
        }
        for (const auto & [port, boundary] : definition.ports)
            ports_.push_back({ instance->work_id,
                               instance->rank,
                               port,
                               local[boundary],
                               std::ranges::find(instance->rank_binding->completion_ports, port) != instance->rank_binding->completion_ports.end() });

        {
            const auto invocation = std::ranges::find(invocations, instance->work_id, &InvokeStep::work_id);
            if (invocation->step == StepKind::Schedule && instance->topology.incoming_request_ready().size() != invocation->input->incoming_requests.size()
                && !assembly_gap_)
                assembly_gap_ = ExecutionTimingGap{ "incoming_request_ready_cohort_not_bound", instance->work_id, instance->rank, std::nullopt };
            std::set<std::size_t> slots;
            for (const auto & [slot, point] : instance->topology.incoming_request_ready()) {
                if (!slots.insert(slot).second || slot >= invocation->input->incoming_requests.size()) {
                    if (!assembly_gap_)
                        assembly_gap_ = ExecutionTimingGap{ "incoming_request_ready_slot_not_bound", instance->work_id, instance->rank, std::nullopt };
                    continue;
                }
                interfaces.push_back(local[point]);
                inputs_.push_back({ instance->work_id,
                                    instance->rank,
                                    local[point],
                                    slot,
                                    invocation->input->incoming_requests[slot].request_id,
                                    instance->rank_binding->receives_inputs });
            }
        }

        if (definition.output_boundary && instance->rank_binding->publishes_outputs) {
            const auto invocation = std::ranges::find(invocations, instance->work_id, &InvokeStep::work_id);
            const auto boundary = local[*definition.output_boundary];
            const auto first_output = outputs_.size();
            for (std::size_t slot = 0; slot < invocation->input->requests.size(); ++slot) {
                const auto & request = invocation->input->requests[slot];
                const auto output = definition.output_policy->evaluate(request.generated_tokens, request.finishes);
                if (output.emits)
                    outputs_.push_back({
                        instance->work_id,
                        instance->rank,
                        boundary,
                        slot,
                        { request.request_id, output.tokens, output.previous_tokens + output.tokens, request.finishes }
                    });
            }

            // An empty message has no request handoff to split CPU service.
            if (outputs_.size() != first_output) interfaces.push_back(boundary);
        }

        for (const auto & [boundary, binding] : instance->resource_imports) {
            if (binding.rule == TemplateConnectionRule::Unresolved) {
                interfaces.push_back(local[boundary]);
                unresolved_.push_back({ instance->work_id, instance->rank, std::nullopt, std::nullopt });
                continue;
            }
            if (binding.peers.empty()) interfaces.push_back(local[boundary]);
            const auto domain = instance->lane(instance->boundary(boundary).lane).resource().domain;
            const auto relation = domain == SourceDomain::CpuThread ? SourceRelation::ThreadOrder : SourceRelation::StreamOrder;
            for (const auto & peer : binding.peers) {
                const auto remote = resolve(peer);
                constraints.emplace(remote, local[boundary], relation);
            }
        }
        // Replacement resource seams carry FIFO order. The actual dependency
        // is sufficient; a private seam does not introduce a new CPU cost cut.
        for (const auto & [peer, boundary] : instance->resource_dependencies) {
            const auto domain = instance->lane(instance->boundary(boundary).lane).resource().domain;
            const auto relation = domain == SourceDomain::CpuThread ? SourceRelation::ThreadOrder : SourceRelation::StreamOrder;
            constraints.emplace(resolve(peer), local[boundary], relation);
        }

        for (const auto & target : instance->connections) {
            const auto & source = target.connection;
            const auto & binding = target.binding;
            if (binding.rule == TemplateConnectionRule::Unresolved) {
                interfaces.push_back(local[source.local_boundary]);
                unresolved_.push_back({ instance->work_id, instance->rank, source.source_edge, std::nullopt });
                continue;
            }

            // Initial resources and exports do not invent zero-duration service
            // nodes. Their meaning is the declared start state or future imports.
            if (binding.peers.empty()) interfaces.push_back(local[source.local_boundary]);
            for (const auto & peer : binding.peers) {
                const auto remote = resolve(peer);
                const auto here = local[source.local_boundary];
                if (source.direction == TemplateConnectionDirection::Incoming) constraints.emplace(remote, here, source.relation);
                else constraints.emplace(here, remote, source.relation);
            }
        }

        for (const auto & target : instance->stream_waits) {
            const auto & wait = target.wait;
            const auto & binding = target.binding;
            if (binding.rule == TemplateConnectionRule::Unresolved) {
                unresolved_.push_back({ instance->work_id, instance->rank, std::nullopt, wait.source_span });
                continue;
            }
            for (const auto & peer : binding.peers) constraints.emplace(resolve(peer), local[wait.completion], SourceRelation::NativeHostWait);
        }

        for (const auto & port : instance->topology.collective_interfaces()) {
            // These observed CPU cuts bound a cost context, not another CPU
            // charge. Preserve them while ordinary CPU services are grouped.
            interfaces.push_back(local[port.host.begin]);
            interfaces.push_back(local[port.host.end]);
        }
        for (const auto & group : instance->collective_groups) {
            std::vector<BoundaryId> ready;
            for (const auto & peer : group.ready) ready.push_back(resolve(peer));
            std::vector<std::tuple<TemplateInstance *, const TemplateCollectiveInstance *, BoundaryId>> participants;
            for (const auto & peer : group.done) {
                const auto participant = *std::ranges::find_if(instances, [&](const auto * candidate) {
                    return candidate->work_id == peer.work_id && candidate->rank == peer.rank;
                });
                const auto & ports = participant->topology.collective_interfaces();
                const auto & port = *std::ranges::find_if(ports, [&](const auto & candidate) { return candidate.work == &group.work; });
                participants.emplace_back(participant, &port, resolve(peer));
            }

            // These zero-work max gates name their readiness contributors;
            // they add neither service nor a distinct native entry event.
            // Device collectives share a qualified gate. A host broadcast uses
            // its sender's data and this participant's entry only.
            const bool host_message = group.work.host_message.has_value();
            const auto common_begin = boundaries_.size();
            if (!host_message) {
                boundaries_.push_back({ RuleBoundary{ instance->work_id }, release_ns });
                for (const auto point : ready) constraints.emplace(point, common_begin, SourceRelation::CompletionJoin);
            }
            for (const auto & [participant, port, output] : participants) {
                auto begin = common_begin;
                if (host_message) {
                    begin = boundaries_.size();
                    boundaries_.push_back({ RuleBoundary{ participant->work_id }, release_ns });
                    for (const auto & peer : group.ready)
                        if (peer.rank == participant->rank || peer.rank == group.work.host_message->root)
                            constraints.emplace(resolve(peer), begin, SourceRelation::CompletionJoin);
                }
                const auto end = host_message ? output : boundaries_.size();
                if (!host_message) {
                    boundaries_.push_back({ RuleBoundary{ participant->work_id }, release_ns });
                    constraints.emplace(end, output, SourceRelation::ModelDataDependency);
                    device_work[{ participant->work_id, participant->rank }].insert(output);
                }
                nodes_.push_back({ .work = participant->work_id,
                                   .rank = participant->rank,
                                   .source_asset = register_source(participant->definition->source_manifest),
                                   .source = std::nullopt,
                                   .role = TemplateNodeRole::Service,
                                   .kind = host_message ? SourceSpanKind::HostCommunication : SourceSpanKind::DeviceMarker,
                                   .begin = begin,
                                   .end = end,
                                   .collective = &group.work });
                if (diagnostics_.is_open()) logical_scopes_.push_back(collective_scope(*participant->definition, port, "operation"));
            }
            // Remote readiness constrains completion, not the start of this
            // rank's conditional envelope or its ordinary enqueue CPU work.
            for (const auto & [entry, done] : group.completion_entry_dependencies)
                constraints.emplace(resolve(entry), resolve(done), SourceRelation::NativeHostWait);
        }
    }

    for (const auto & port : ports_) gates_.emplace(Gate{ port.work, port.rank, port.kind }, port.boundary);
    for (const auto & invocation : invocations) {
        for (const auto * instance : instances) {
            if (instance->work_id != invocation.work_id) continue;
            const auto & definition = *instance->definition;
            const auto entry = resolve(TemplateEndpoint{ instance->work_id, instance->rank, definition.entry });
            for (const auto & dependency : invocation.dependencies) {
                constraints.emplace(resolve(Gate{ dependency.work_id, instance->rank, dependency.port }), entry, std::nullopt);
            }
            for (const auto & dependency : invocation.port_dependencies) {
                constraints.emplace(resolve(Gate{ dependency.predecessor.work_id, instance->rank, dependency.predecessor.port }),
                                    resolve(Gate{ invocation.work_id, instance->rank, dependency.target }),
                                    std::nullopt);
            }
        }
    }

    // DeviceDone joins the work that exists after every rewrite. Old task ends
    // can remain as zero-work resource aliases, but no longer denote submitted
    // device work; inserted layers and logical collectives contribute instead.
    std::set<BoundaryId> device_completions;
    for (const auto & port : ports_)
        if (port.kind == PortKind::DeviceDone) device_completions.insert(port.boundary);
    std::erase_if(constraints,
                  [&](const auto & edge) { return std::get<2>(edge) == SourceRelation::CompletionJoin && device_completions.contains(std::get<1>(edge)); });
    for (const auto & port : ports_)
        if (port.kind == PortKind::DeviceDone)
            for (const auto point : device_work.at({ port.work, port.rank })) constraints.emplace(point, port.boundary, SourceRelation::CompletionJoin);

    // A service's own extent is causal even before a duration is supplied.
    // Keep it in the same graph used by cycle checks and CPU partitioning.
    for (const auto & node : nodes_) constraints.emplace(node.begin, node.end, SourceRelation::IntervalExtent);
    for (const auto & [source, target, relation] : constraints) {
        if (source == target) continue;
        if (!std::holds_alternative<ObservedBoundary>(boundaries_[target].origin) && !std::holds_alternative<RuleBoundary>(boundaries_[target].origin))
            throw std::logic_error("new work cannot modify a committed boundary");
        edges_.push_back({ source, target, relation });
    }

    acyclic_ = acyclic_ && acyclic();
    // The partition traverses serial paths; reject a cyclic assembly first.
    if (acyclic_) group_cpu_services(instances, invocations, interfaces);
    return acyclic_;
}

const nlohmann::json * ExecutionGraph::workspace_effect(const TemplateInstance & instance, const TemplateNode & node) const {
    if (!diagnostics_.is_open() || node.kind != SourceSpanKind::ResourceObservation) return nullptr;
    const nlohmann::json * effect = nullptr;
    for (const auto & use : instance.workspace_uses) {
        if (use.begin != node.begin || use.end != node.end) continue;
        if (effect) return nullptr;
        effect = &use.effect;
    }
    return effect;
}

bool ExecutionGraph::acyclic() const { return DirectedGraph(boundaries_.size(), edges_).acyclic(); }

CostQuery ExecutionGraph::Node::cost_query() const {
    return CostQuery::from_work({ kind, kernel, transfer, device_command, cpu ? &*cpu : nullptr, collective, rank });
}


std::expected<std::vector<ExecutionEvent>, ExecutionTimingGap> ExecutionGraph::schedule(const NodeCosts & costs, std::span<const InvokeStep> invocations) {
    if (assembly_gap_) return std::unexpected(*assembly_gap_);
    if (!unresolved_.empty()) {
        const auto & missing = unresolved_.front();
        return std::unexpected(ExecutionTimingGap{ missing.source_span ? "target_stream_wait_unresolved" : "target_connection_unresolved",
                                                   missing.work,
                                                   missing.rank,
                                                   missing.source_span });
    }

    struct TimedEdge {
        BoundaryId source;
        BoundaryId target;
        std::int64_t duration_ns;
    };
    std::vector<TimedEdge> constraints;
    std::map<WorkId, std::shared_ptr<const nlohmann::json>> origins;
    std::optional<ExecutionTimingGap> first_gap;
    constraints.reserve(edges_.size() + nodes_.size());
    for (const auto & edge : edges_) constraints.push_back({ edge.source, edge.target, 0 });
    for (auto & node : nodes_) {
        if (node.role != TemplateNodeRole::Service) continue;
        const auto & invocation = *std::ranges::find(invocations, node.work, &InvokeStep::work_id);
        const auto query = node.cost_query();
        const auto measured = costs.query_cost(query);
        requirements_.record(query, measured, invocation, origins[node.work]);

        // Coverage retains the measured result even when explicit given
        // durations let this otherwise unmeasured work advance the clock.
        const auto & manifest = sources_[node.source_asset];
        const auto duration = costs.estimate(measured, query, manifest);
        if (!duration) {
            if (!first_gap) first_gap = ExecutionTimingGap{ std::string(duration.error()), node.work, node.rank, node.source, manifest, query.describe() };
            continue;
        }

        node.cost = *duration;
        constraints.push_back({ node.begin, node.end, duration->duration_ns });
    }
    // Unknown costs never enter the event clock. Analyze the complete assembled
    // frontier first, then return the first execution location plus all demands.
    if (first_gap) return std::unexpected(std::move(*first_gap));

    // Waiting and annotations contribute dependencies, not service charges.
    // The runtime supplies release times and remains the sole event clock.
    const DirectedGraph graph(boundaries_.size(), constraints);
    if (!graph.acyclic()) throw std::logic_error("timed graph contains a dependency cycle");

    completion_ns_.clear();
    for (const auto & boundary : boundaries_) completion_ns_.push_back(boundary.release_ns);
    for (const auto point : graph.order()) {
        for (const auto index : graph.outgoing(point)) {
            const auto & edge = constraints[index];
            if (edge.duration_ns > std::numeric_limits<std::int64_t>::max() - completion_ns_[point]) {
                throw std::overflow_error("node execution time exceeds int64 nanoseconds");
            }
            completion_ns_[edge.target] = std::max(completion_ns_[edge.target], completion_ns_[point] + edge.duration_ns);
        }
    }

    // Control joins and actual token handoffs have separate boundaries.
    std::map<std::pair<WorkId, PortKind>, std::int64_t> completions;
    for (const auto & port : ports_) {
        const auto time = completion_ns_[port.boundary];
        if (!port.notifies_controller) continue;
        auto & eligible = completions[{ port.work, port.kind }];
        eligible = std::max(eligible, time);
    }
    std::vector<ExecutionEvent> events;
    for (const auto & [port, time] : completions) { events.push_back({ time, WorkPortCompleted{ { port.first, port.second } } }); }
    for (const auto & input : inputs_)
        if (input.notifies_runtime) events.push_back({ completion_ns_[input.boundary], RequestReady{ input.request_id } });
    std::map<std::pair<WorkId, RequestId>, ExecutionEvent> publications;
    for (const auto & output : outputs_) {
        const auto [entry, inserted] =
            publications.try_emplace(std::pair{ output.work, output.effect.request_id }, ExecutionEvent{ completion_ns_[output.boundary], output.effect });
        if (!inserted) entry->second.time_ns = std::max(entry->second.time_ns, completion_ns_[output.boundary]);
    }
    for (auto & [key, event] : publications) events.push_back(std::move(event));
    return events;
}

void ExecutionGraph::write_batch(const nlohmann::json & invocations) {
    auto & stream = diagnostics_;
    // Each line owns one admitted graph. Historical resource and controller
    // references are explicit fixed-time inputs, not duplicated old subgraphs.
    stream << "{\"record\":\"batch\",\"batch\":" << batches_ << ",\"invocations\":" << invocations << ",\"instances\":[";
    bool first_instance = true;
    for (const auto & [instance, points] : instance_boundaries_) {
        if (!first_instance) stream << ',';
        first_instance = false;
        stream << nlohmann::json({
            {    "work_id",  instance.first },
            {       "rank", instance.second },
            { "boundaries",          points }
        });
    }
    stream << "],\"boundaries\":[";
    for (std::size_t index = 0; index < boundaries_.size(); ++index) {
        const auto & boundary = boundaries_[index];
        if (index != 0) stream << ',';
        nlohmann::json record{
            {    "release_ns",                                                                             boundary.release_ns },
            { "completion_ns", index < completion_ns_.size() ? nlohmann::json(completion_ns_[index]) : nlohmann::json(nullptr) }
        };
        if (const auto * source = std::get_if<ObservedBoundary>(&boundary.origin)) {
            record["source_boundary"] = source->source;
            record["source_asset"] = source->source_asset;
        }
        else if (const auto * predecessor = std::get_if<TemplateEndpoint>(&boundary.origin)) {
            record["resource_predecessor"] = {
                {  "work_id",  predecessor->work_id },
                {     "rank",     predecessor->rank },
                { "boundary", predecessor->boundary }
            };
        }
        else if (const auto * generated = std::get_if<RuleBoundary>(&boundary.origin)) { record["generated_for_work"] = generated->work; }
        else {
            const auto & gate = std::get<Gate>(boundary.origin);
            record["port_predecessor"] = {
                { "work_id",                              gate.work },
                {    "rank",                              gate.rank },
                {    "port", ServingContract{}.port_name(gate.kind) }
            };
        }
        stream << record;
    }

    stream << "],\"nodes\":[";
    for (std::size_t index = 0; index < nodes_.size(); ++index) {
        const auto & node = nodes_[index];
        if (index != 0) stream << ',';
        auto record = nlohmann::json({
            {       "work_id",                                                            node.work },
            {          "rank",                                                            node.rank },
            {  "source_asset",                                                    node.source_asset },
            {   "source_span", node.source ? nlohmann::json(*node.source) : nlohmann::json(nullptr) },
            {          "role",                                                 role_name(node.role) },
            {          "kind",                                    SourceGraph::span_name(node.kind) },
            {         "begin",                                                           node.begin },
            {           "end",                                                             node.end },
            { "logical_scope",                                            logical_scopes_.at(index) },
            {        "kernel",      node.kernel ? node.kernel->describe() : nlohmann::json(nullptr) }
        });
        // Failed scheduling does not commit the staged allocator effects.
        if (node.kind == SourceSpanKind::ResourceObservation)
            record["workspace"] = node.workspace && completion_ns_.size() == boundaries_.size() ? *node.workspace : nlohmann::json(nullptr);
        if (node.transfer) record["transfer"] = node.transfer->describe();
        if (node.device_command) record["device_command"] = node.device_command->describe();
        if (node.cpu) {
            record["cpu"] = node.cpu->describe();
            record["cpu_resource"] = node.cpu_resource->describe();
        }
        if (node.collective && node.kind != SourceSpanKind::CpuControl) record["collective"] = node.cost_query().describe().at("features");
        if (node.input_slot) record["input_slot"] = *node.input_slot;
        if (node.cost) {
            record["service_cost"] = {
                { "duration_ns",                                                                     node.cost->duration_ns },
                {       "basis",                                                    NodeCosts::basis_name(node.cost->basis) },
                { "model_index", node.cost->model_index ? nlohmann::json(*node.cost->model_index) : nlohmann::json(nullptr) }
            };
        }
        stream << record;
    }

    stream << "],\"edges\":[";
    for (std::size_t index = 0; index < edges_.size(); ++index) {
        const auto & edge = edges_[index];
        if (index != 0) stream << ',';
        stream << nlohmann::json({
            {   "source",                                                                                          edge.source },
            {   "target",                                                                                          edge.target },
            { "relation", edge.relation ? nlohmann::json(SourceGraph::relation_name(*edge.relation)) : nlohmann::json(nullptr) }
        });
    }

    stream << "],\"ports\":[";
    const ServingContract contract;
    for (std::size_t index = 0; index < ports_.size(); ++index) {
        const auto & port = ports_[index];
        if (index != 0) stream << ',';
        stream << nlohmann::json({
            {             "work_id",                                                                                                       port.work },
            {                "rank",                                                                                                       port.rank },
            {                "port",                                                                                   contract.port_name(port.kind) },
            {            "boundary",                                                                                                   port.boundary },
            { "notifies_controller",                                                                                        port.notifies_controller },
            {       "completion_ns", port.boundary < completion_ns_.size() ? nlohmann::json(completion_ns_[port.boundary]) : nlohmann::json(nullptr) }
        });
    }
    stream << "],\"outputs\":[";
    for (std::size_t index = 0; index < outputs_.size(); ++index) {
        const auto & output = outputs_[index];
        if (index != 0) stream << ',';
        stream << nlohmann::json({
            {           "work_id",                                                                                                         output.work },
            {              "rank",                                                                                                         output.rank },
            {          "boundary",                                                                                                     output.boundary },
            {      "request_slot",                                                                                                 output.request_slot },
            {            "tokens",                                                                                                output.effect.tokens },
            { "cumulative_tokens",                                                                                     output.effect.cumulative_tokens },
            {          "finishes",                                                                                              output.effect.finishes },
            {     "completion_ns", output.boundary < completion_ns_.size() ? nlohmann::json(completion_ns_[output.boundary]) : nlohmann::json(nullptr) }
        });
    }
    stream << "],\"request_ready\":[";
    for (std::size_t index = 0; index < inputs_.size(); ++index) {
        const auto & input = inputs_[index];
        if (index != 0) stream << ',';
        stream << nlohmann::json({
            {          "work_id",                                                                                                        input.work },
            {             "rank",                                                                                                        input.rank },
            {         "boundary",                                                                                                    input.boundary },
            {       "input_slot",                                                                                                  input.input_slot },
            {       "request_id",                                                                                                  input.request_id },
            { "notifies_runtime",                                                                                            input.notifies_runtime },
            {    "completion_ns", input.boundary < completion_ns_.size() ? nlohmann::json(completion_ns_[input.boundary]) : nlohmann::json(nullptr) }
        });
    }
    stream << "]}\n";
}

void ExecutionGraph::record_batch(const nlohmann::json & invocations) {
    total_boundaries_ += static_cast<std::size_t>(std::ranges::count_if(boundaries_, [](const auto & boundary) {
        return std::holds_alternative<ObservedBoundary>(boundary.origin) || std::holds_alternative<RuleBoundary>(boundary.origin);
    }));
    total_nodes_ += nodes_.size();
    total_edges_ += edges_.size();
    total_ports_ += ports_.size();
    peak_boundaries_ = std::max(peak_boundaries_, boundaries_.size());
    peak_nodes_ = std::max(peak_nodes_, nodes_.size());
    unresolved_count_ += unresolved_.size();

    for (const auto & node : nodes_) {
        ++node_roles_[std::string(role_name(node.role))];
        if (!node.cost) continue;

        const auto basis = std::string(NodeCosts::basis_name(node.cost->basis));
        ++cost_usage_[basis];
        std::string work = std::string(SourceGraph::span_name(node.kind));
        if (node.collective && node.kind != SourceSpanKind::CpuControl) work = "collective";
        else if (node.transfer) work = "transfer";
        else if (node.kernel && node.kernel->attention) work = "attention";
        ++cost_usage_by_work_[work][basis];
    }
    if (diagnostics_.is_open()) write_batch(invocations);
    ++batches_;
}

void ExecutionGraph::finish() {
    if (!diagnostics_.is_open()) return;

    diagnostics_.close();
    auto temporary = *path_;
    temporary += ".tmp";
    std::filesystem::rename(temporary, *path_);
}

nlohmann::json ExecutionGraph::describe() const {
    auto sources = nlohmann::json::array();
    for (const auto & manifest : sources_) sources.push_back(manifest.string());
    return {
        {                     "sources",     std::move(sources) },
        {                     "batches",               batches_ },
        {                  "boundaries",      total_boundaries_ },
        {                       "nodes",           total_nodes_ },
        {                       "edges",           total_edges_ },
        {                       "ports",           total_ports_ },
        {                     "acyclic",               acyclic_ },
        {                  "cost_usage",            cost_usage_ },
        {          "cost_usage_by_work",    cost_usage_by_work_ },
        {                  "node_roles",            node_roles_ },
        {      "unresolved_connections",      unresolved_count_ },
        {            "peak_batch_nodes",            peak_nodes_ },
        {       "peak_batch_boundaries",       peak_boundaries_ },
        { "retained_resource_endpoints", resource_times_.size() },
        {      "retained_control_ports",     port_times_.size() }
    };
}

} // namespace markov
