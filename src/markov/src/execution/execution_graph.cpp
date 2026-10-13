#include "markov/execution/execution_graph.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <utility>

namespace markov {

BoundaryId ExecutionGraph::resolve(const TemplateEndpoint & endpoint) const {
    return instance_boundaries_.at({ endpoint.work_id, endpoint.rank }).at(endpoint.boundary);
}

std::size_t ExecutionGraph::register_source(const std::filesystem::path & manifest) {
    const auto found = std::ranges::find(sources_, manifest);
    if (found != sources_.end()) return static_cast<std::size_t>(found - sources_.begin());
    sources_.push_back(manifest);
    return sources_.size() - 1;
}

bool ExecutionGraph::append(std::span<TemplateInstance *> instances, std::span<const InvokeStep> invocations, std::int64_t release_ns) {
    const auto first_node = nodes_.size();
    pending_port_ = ports_.size();
    std::map<std::pair<std::size_t, BoundaryId>, BoundaryId> shared;
    for (const auto * instance : instances) {
        const auto & definition = *instance->definition;
        const auto source_asset = register_source(definition.source_manifest);
        auto & local = instance_boundaries_[{ instance->work_id, definition.rank }];
        local.reserve(definition.graph.boundary_count());
        for (const auto & boundary : definition.boundary_observations) {
            const auto [position, inserted] = shared.try_emplace(std::pair{ source_asset, boundary.source_boundary }, boundaries_.size());
            if (inserted) boundaries_.push_back({ boundary.source_boundary, source_asset, batches_, release_ns });
            local.push_back(position->second);
        }
        // Donor provenance does not grant sharing with any other insertion.
        for (const auto & boundary : instance->topology.inserted_boundaries()) {
            local.push_back(boundaries_.size());
            boundaries_.push_back({ boundary.source_boundary, register_source(*boundary.source_manifest), batches_, release_ns });
        }
    }

    // A source point is shared within this batch only. Reusing the same template
    // in a later iteration creates new target points instead of aliasing old work.
    std::set<std::pair<std::size_t, SourceSpanId>> members;
    std::set<std::tuple<BoundaryId, BoundaryId, std::optional<SourceRelation>>> constraints;
    for (const auto * instance : instances) {
        const auto & definition = *instance->definition;
        const auto source_asset = register_source(definition.source_manifest);
        const auto & local = instance_boundaries_.at({ instance->work_id, definition.rank });
        for (const auto index : instance->topology.nodes()) {
            const auto & node = definition.graph[index];
            const auto & node_observation = definition.observation(node);
            if (members.emplace(source_asset, node_observation.source_span).second) {
                const auto member = instance->publication_member_slots.find(node_observation.source_span);
                const bool member_service = member != instance->publication_member_slots.end();
                nodes_.push_back({ instance->work_id,
                                   definition.rank,
                                   source_asset,
                                   node_observation.source_span,
                                   node.role(),
                                   node.kind,
                                   instance->kernel(node),
                                   instance->communication(node),
                                   local[node.begin],
                                   local[node.end],
                                   instance->transfer(node),
                                   instance->publication_work && (instance->publication_work->source_span == node_observation.source_span || member_service)
                                       ? &*instance->publication_work
                                       : nullptr,
                                   member_service ? std::optional(member->second) : std::nullopt,
                                   node_observation.control_part,
                                   node.kind == SourceSpanKind::CpuControl ? definition.admission_member_slot(node_observation.source_span) : std::nullopt,
                                   node_observation.cpu_work ? std::optional(CpuSubmissionWork{ &*node_observation.cpu_work, {} }) : std::nullopt });
            }
        }

        // Inserted work has target-local endpoints. Its source span is cost
        // provenance, not an identity to deduplicate against other instances.
        for (const auto & inserted : instance->topology.inserted_nodes()) {
            const auto & node = inserted.node;
            const bool member_service = instance->publication_work && node.kind == SourceSpanKind::CpuControl && inserted.request_slot.has_value();
            nodes_.push_back({ instance->work_id,
                               definition.rank,
                               register_source(inserted.definition->source_manifest),
                               inserted.observation().source_span,
                               node.role(),
                               node.kind,
                               inserted.kernel(),
                               inserted.communication(),
                               local[node.begin],
                               local[node.end],
                               inserted.transfer(),
                               member_service ? &*instance->publication_work : nullptr,
                               member_service ? inserted.request_slot : std::nullopt,
                               inserted.observation().control_part,
                               node.kind == SourceSpanKind::CpuControl && inserted.definition->admission_member_slot(inserted.observation().source_span)
                                   ? inserted.request_slot
                                   : std::nullopt,
                               inserted.observation().cpu_work ? std::optional(CpuSubmissionWork{ &*inserted.observation().cpu_work, {} }) : std::nullopt });
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
        for (const auto & [port, boundary] : definition.ports) ports_.push_back({ instance->work_id, definition.rank, port, local[boundary] });

        for (const auto & [boundary, binding] : instance->resource_imports) {
            if (binding.rule == TemplateConnectionRule::Unresolved) {
                unresolved_.push_back({ instance->work_id, definition.rank, std::nullopt, std::nullopt });
                continue;
            }
            for (const auto & peer : binding.peers) constraints.emplace(resolve(peer), local[boundary], std::nullopt);
        }
        for (const auto & [peer, boundary] : instance->resource_dependencies) constraints.emplace(resolve(peer), local[boundary], std::nullopt);

        for (const auto & target : instance->connections) {
            const auto & source = target.connection;
            const auto & binding = target.binding;
            if (binding.rule == TemplateConnectionRule::Unresolved) {
                unresolved_.push_back({ instance->work_id, definition.rank, source.source_edge, std::nullopt });
                continue;
            }

            // Initial resources and exports do not invent zero-duration service
            // nodes. Their meaning is the declared start state or future imports.
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
                unresolved_.push_back({ instance->work_id, definition.rank, std::nullopt, wait.source_span });
                continue;
            }
            for (const auto & peer : binding.peers) constraints.emplace(resolve(peer), local[wait.completion], SourceRelation::NativeHostWait);
        }
    }

    std::map<std::tuple<WorkId, std::uint32_t, PortKind>, BoundaryId> gates;
    for (const auto & port : ports_) gates.emplace(std::tuple(port.work, port.rank, port.kind), port.boundary);
    for (const auto & invocation : invocations) {
        for (const auto * instance : instances) {
            if (instance->work_id != invocation.work_id) continue;
            const auto & definition = *instance->definition;
            const auto entry = resolve({ instance->work_id, definition.rank, definition.entry });
            for (const auto & dependency : invocation.dependencies) {
                constraints.emplace(gates.at({ dependency.work_id, definition.rank, dependency.port }), entry, std::nullopt);
            }
            for (const auto & dependency : invocation.port_dependencies) {
                constraints.emplace(gates.at({ dependency.predecessor.work_id, definition.rank, dependency.predecessor.port }),
                                    gates.at({ invocation.work_id, definition.rank, dependency.target }),
                                    std::nullopt);
            }
        }
    }

    for (const auto & [source, target, relation] : constraints) {
        if (source != target) edges_.push_back({ source, target, relation });
    }

    bind_cpu_submissions(first_node);
    ++batches_;
    acyclic_ = acyclic();
    return acyclic_;
}

void ExecutionGraph::bind_cpu_submissions(std::size_t first_node) {
    std::map<std::tuple<std::size_t, SourceSpanId, BoundaryId>, BoundaryId> call_entries;
    std::map<BoundaryId, std::vector<std::size_t>> submitted_nodes;
    for (auto index = first_node; index < nodes_.size(); ++index) {
        const auto & node = nodes_[index];
        if (node.cpu_submission && node.cpu_submission->observation->part == CpuSubmissionPart::Call)
            call_entries.emplace(std::tuple{ node.source_asset, node.cpu_submission->observation->submission, node.end }, node.begin);
        if (node.kind != SourceSpanKind::CpuControl && node.kind != SourceSpanKind::CpuObservation) submitted_nodes[node.begin].push_back(index);
    }

    std::map<BoundaryId, std::vector<std::size_t>> roots;
    for (const auto & edge : edges_) {
        if (edge.relation != SourceRelation::NativeFlow) continue;
        const auto targets = submitted_nodes.find(edge.target);
        if (targets != submitted_nodes.end()) roots[edge.source].insert(roots[edge.source].end(), targets->second.begin(), targets->second.end());
    }

    for (auto index = first_node; index < nodes_.size(); ++index) {
        auto & node = nodes_[index];
        if (!node.cpu_submission) continue;
        auto & work = *node.cpu_submission;
        const auto & source = *work.observation;
        auto entry = source.part == CpuSubmissionPart::Before ? node.end : node.begin;
        if (source.part == CpuSubmissionPart::After) {
            // The target return endpoint disambiguates repeated donor insertions.
            const auto call = call_entries.find({ node.source_asset, source.submission, node.begin });
            if (call == call_entries.end()) continue;
            entry = call->second;
        }

        const auto submitted = roots.find(entry);
        if (submitted == roots.end()) continue;
        for (const auto target : submitted->second) {
            const auto & device = nodes_[target];
            work.roots.push_back({ device.kind, device.kernel, device.communication, device.transfer });
        }
    }
}

bool ExecutionGraph::acyclic() const {
    std::vector<std::size_t> incoming(boundaries_.size());
    std::vector<std::size_t> offsets(boundaries_.size() + 1);
    for (const auto & edge : edges_) {
        ++incoming[edge.target];
        ++offsets[edge.source + 1];
    }
    for (std::size_t index = 1; index < offsets.size(); ++index) offsets[index] += offsets[index - 1];

    auto positions = offsets;
    std::vector<BoundaryId> successors(edges_.size());
    for (const auto & edge : edges_) successors[positions[edge.source]++] = edge.target;

    std::vector<BoundaryId> ready;
    for (BoundaryId point = 0; point < incoming.size(); ++point) {
        if (incoming[point] == 0) ready.push_back(point);
    }

    for (std::size_t cursor = 0; cursor < ready.size(); ++cursor) {
        const auto point = ready[cursor];
        for (auto edge = offsets[point]; edge < offsets[point + 1]; ++edge) {
            const auto target = successors[edge];
            if (--incoming[target] == 0) ready.push_back(target);
        }
    }

    return ready.size() == boundaries_.size();
}

std::expected<std::vector<ControlEvent>, ExecutionTimingGap> ExecutionGraph::schedule(const NodeCosts & costs,
                                                                                      const std::map<WorkId, const InvokeStep *> & invocations) {
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
    constraints.reserve(edges_.size() + nodes_.size());
    for (const auto & edge : edges_) constraints.push_back({ edge.source, edge.target, 0 });
    for (auto & node : nodes_) {
        if (node.role != TemplateNodeRole::Service) continue;
        const auto & manifest = sources_[node.source_asset];
        const ServiceWork work{ manifest,
                                node.source,
                                node.rank,
                                node.kind,
                                *invocations.at(node.work),
                                node.kernel,
                                node.communication,
                                node.transfer,
                                node.publication,
                                node.publication_member_slot,
                                node.control_part,
                                node.admission_member_slot,
                                node.cpu_submission ? &*node.cpu_submission : nullptr };
        const auto duration = costs.estimate(work);
        if (!duration)
            return std::unexpected(ExecutionTimingGap{ std::string(duration.error()), node.work, node.rank, node.source, manifest, work.describe() });

        node.cost = *duration;
        constraints.push_back({ node.begin, node.end, duration->duration_ns });
    }

    // Waiting and annotations contribute dependencies, not service charges.
    // The runtime supplies release times and remains the sole event clock.
    std::vector<std::size_t> incoming(boundaries_.size());
    std::vector<std::size_t> offsets(boundaries_.size() + 1);
    for (const auto & edge : constraints) {
        ++incoming[edge.target];
        ++offsets[edge.source + 1];
    }
    for (std::size_t index = 1; index < offsets.size(); ++index) offsets[index] += offsets[index - 1];
    auto positions = offsets;
    std::vector<std::size_t> successors(constraints.size());
    for (std::size_t edge = 0; edge < constraints.size(); ++edge) successors[positions[constraints[edge].source]++] = edge;

    completion_ns_.clear();
    for (const auto & boundary : boundaries_) completion_ns_.push_back(boundary.release_ns);
    std::vector<BoundaryId> ready;
    for (BoundaryId point = 0; point < incoming.size(); ++point) {
        if (incoming[point] == 0) ready.push_back(point);
    }
    for (std::size_t cursor = 0; cursor < ready.size(); ++cursor) {
        const auto point = ready[cursor];
        for (auto index = offsets[point]; index < offsets[point + 1]; ++index) {
            const auto & edge = constraints[successors[index]];
            if (edge.duration_ns > std::numeric_limits<std::int64_t>::max() - completion_ns_[point]) {
                throw std::overflow_error("node execution time exceeds int64 nanoseconds");
            }
            completion_ns_[edge.target] = std::max(completion_ns_[edge.target], completion_ns_[point] + edge.duration_ns);
            if (--incoming[edge.target] == 0) ready.push_back(edge.target);
        }
    }
    if (ready.size() != boundaries_.size()) throw std::logic_error("timed graph contains a dependency cycle");

    // A public port completes once all its rank-local boundaries complete.
    std::map<std::pair<WorkId, PortKind>, std::int64_t> completions;
    for (auto index = pending_port_; index < ports_.size(); ++index) {
        const auto & port = ports_[index];
        auto & time = completions[{ port.work, port.kind }];
        time = std::max(time, completion_ns_[port.boundary]);
    }
    std::vector<ControlEvent> events;
    for (const auto & [port, time] : completions) events.push_back({ time, WorkPortCompleted{ { port.first, port.second } } });
    return events;
}

void ExecutionGraph::write(const std::filesystem::path & path) const {
    auto temporary = path;
    temporary += ".tmp";
    std::ofstream stream(temporary);
    stream.exceptions(std::ios::failbit | std::ios::badbit);

    // Stream the actual assembled graph without constructing another full JSON
    // graph in memory. Array indices are target identities within this candidate.
    auto sources = nlohmann::json::array();
    for (const auto & manifest : sources_) sources.push_back(manifest.string());
    stream << "{\"sources\":" << sources << ",\"boundaries\":[";
    for (std::size_t index = 0; index < boundaries_.size(); ++index) {
        const auto & boundary = boundaries_[index];
        if (index != 0) stream << ',';
        stream << nlohmann::json({
            { "source_boundary",                                                                                 boundary.source },
            {    "source_asset",                                                                           boundary.source_asset },
            {           "batch",                                                                                  boundary.batch },
            {      "release_ns",                                                                             boundary.release_ns },
            {   "completion_ns", index < completion_ns_.size() ? nlohmann::json(completion_ns_[index]) : nlohmann::json(nullptr) }
        });
    }

    stream << "],\"nodes\":[";
    for (std::size_t index = 0; index < nodes_.size(); ++index) {
        const auto & node = nodes_[index];
        std::string_view role;
        switch (node.role) {
        case TemplateNodeRole::Service:
            role = "service";
            break;
        case TemplateNodeRole::Wait:
            role = "wait";
            break;
        case TemplateNodeRole::Annotation:
            role = "annotation";
            break;
        }

        if (index != 0) stream << ',';
        auto record = nlohmann::json({
            {      "work_id",                                                       node.work },
            {         "rank",                                                       node.rank },
            { "source_asset",                                               node.source_asset },
            {  "source_span",                                                     node.source },
            {         "role",                                                            role },
            {         "kind",                               SourceGraph::span_name(node.kind) },
            {        "begin",                                                      node.begin },
            {          "end",                                                        node.end },
            {       "kernel", node.kernel ? node.kernel->describe() : nlohmann::json(nullptr) }
        });
        if (node.communication) record["communication"] = node.communication->describe();
        if (node.transfer) record["transfer"] = node.transfer->describe();
        if (node.cpu_submission) record["cpu_submission"] = node.cpu_submission->describe();
        if (node.publication_member_slot) {
            record["publication_member_work"] = node.publication->members[*node.publication_member_slot].describe();
            record["publication_member_work"]["slot"] = *node.publication_member_slot;
        }
        else if (node.publication) { record["publication_work"] = node.publication->describe(); }
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
            {  "work_id",                     port.work },
            {     "rank",                     port.rank },
            {     "port", contract.port_name(port.kind) },
            { "boundary",                 port.boundary }
        });
    }
    stream << "]}\n";
    stream.close();
    std::filesystem::rename(temporary, path);
}

nlohmann::json ExecutionGraph::describe() const {
    auto ports = nlohmann::json::array();
    const ServingContract contract;
    for (const auto & port : ports_) {
        ports.push_back({
            {         "work_id",                                                                                                       port.work },
            {            "rank",                                                                                                       port.rank },
            {            "port",                                                                                   contract.port_name(port.kind) },
            {        "boundary",                                                                                                   port.boundary },
            { "source_boundary",                                                                               boundaries_[port.boundary].source },
            {    "source_asset",                                                                         boundaries_[port.boundary].source_asset },
            {           "batch",                                                                                boundaries_[port.boundary].batch },
            {   "completion_ns", port.boundary < completion_ns_.size() ? nlohmann::json(completion_ns_[port.boundary]) : nlohmann::json(nullptr) }
        });
    }

    auto unresolved = nlohmann::json::array();
    for (const auto & edge : unresolved_) {
        unresolved.push_back({
            {     "work_id",                                                                      edge.work },
            {        "rank",                                                                      edge.rank },
            { "source_edge", edge.source_edge ? nlohmann::json(*edge.source_edge) : nlohmann::json(nullptr) },
            { "source_span", edge.source_span ? nlohmann::json(*edge.source_span) : nlohmann::json(nullptr) }
        });
    }

    std::size_t services = 0;
    std::size_t waits = 0;
    std::size_t annotations = 0;
    auto cost_usage = nlohmann::json::object();
    for (const auto & node : nodes_) {
        if (node.cost) {
            const auto name = std::string(NodeCosts::basis_name(node.cost->basis));
            cost_usage[name] = cost_usage.value(name, std::size_t{ 0 }) + 1;
        }

        switch (node.role) {
        case TemplateNodeRole::Service:
            ++services;
            break;
        case TemplateNodeRole::Wait:
            ++waits;
            break;
        case TemplateNodeRole::Annotation:
            ++annotations;
            break;
        }
    }

    auto sources = nlohmann::json::array();
    for (const auto & manifest : sources_) sources.push_back(manifest.string());
    nlohmann::json result = {
        {                "sources",                                                                       sources },
        {                "batches",                                                                      batches_ },
        {             "boundaries",                                                            boundaries_.size() },
        {                  "nodes",                                                                 nodes_.size() },
        {                  "edges",                                                                 edges_.size() },
        {                "acyclic",                                                                      acyclic_ },
        {             "cost_usage",                                                                    cost_usage },
        {             "node_roles", { { "service", services }, { "wait", waits }, { "annotation", annotations } } },
        {                  "ports",                                                                         ports },
        { "unresolved_connections",                                                                    unresolved }
    };
    return result;
}

} // namespace markov
