#include "markov/adapters/frameworks/sglang/template_bindings.hpp"

#include "markov/production/source_call.hpp"
#include "markov/templates/templates.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace markov {

void SGLangTemplateBindings::bind_model_sequence(std::uint32_t target_layers) {
    if (definition_.step != StepKind::Prefill && definition_.step != StepKind::Decode) return;

    std::map<std::int64_t, std::size_t> observed;
    for (std::size_t index = 0; index < definition_.regions.size(); ++index) {
        const auto & region = definition_.regions[index];
        if (region.semantic == "decoder_layer") observed.emplace(native_regions_.at(region.source_call).parameters.at("layer_index"), index);
    }
    if (observed.size() == target_layers) return;
    if (observed.size() != 3 || target_layers < observed.size())
        throw std::invalid_argument("representative dense model requires first, middle and last decoder layers");

    // The native dense constructor distinguishes first/last scatter modes.
    // Retain both interfaces and repeat only the homogeneous interior body.
    TemplateRegionSequence sequence;
    for (const auto & [layer, index] : observed) sequence.source_members.push_back(index);
    sequence.members.push_back(0);
    for (std::uint32_t layer = 1; layer + 1 < target_layers; ++layer) sequence.members.push_back(1);
    sequence.members.push_back(sequence.source_members.size() - 1);
    definition_.model_sequence =
        definition_.declare_sequence(std::move(sequence.source_members), TemplateSequenceParameter::ModelLayer, std::move(sequence.members));
}

std::unexpected<TemplateGap> SGLangTemplateBindings::gap(TemplateGapKind kind, std::string reason) const {
    return std::unexpected(TemplateGap{ kind, std::move(reason), definition_.step, definition_.phase, definition_.rank });
}

std::expected<void, TemplateGap> SGLangTemplateBindings::bind_input_messages(std::uint32_t logical_tp_size, std::span<const SourceCall> calls,
                                                                             const nlohmann::json & records) {
    if (definition_.step != StepKind::Schedule || logical_tp_size == 1) return {};

    std::set<std::uint32_t> ordinals;
    for (std::size_t index = 0; index < definition_.regions.size(); ++index) {
        const auto & region = definition_.regions[index];
        if (region.semantic != "input_message") continue;
        const auto parent = std::ranges::find(definition_.regions, region.parent_call, &TemplateRegion::source_call);
        if (parent == definition_.regions.end() || parent->semantic != "input_broadcast" || !region.cpu_scope)
            return gap(TemplateGapKind::Observation, "input_message_native_parent_not_observed");
        const auto & metadata = records.at(calls[region.source_call].identity).at("metadata").at("message");
        const auto ranks = metadata.at("group_ranks").get<std::vector<std::uint32_t>>();
        const auto root = metadata.at("root").get<std::uint32_t>();
        const auto ordinal = metadata.at("ordinal").get<std::uint32_t>();
        const auto physical = metadata.at("physical_group_size").get<std::uint32_t>();
        const auto channel = metadata.at("channel").get<std::string>();
        bool group = ranks.size() == logical_tp_size;
        for (std::uint32_t rank = 0; rank < ranks.size(); ++rank) group = group && ranks[rank] == rank;
        if (!group || root >= logical_tp_size || metadata.at("rank") != definition_.rank || !metadata.at("synchronous").get<bool>()
            || (physical != 1 && physical != logical_tp_size))
            return gap(TemplateGapKind::Rule, "input_message_native_group_not_qualified");
        if (!ordinals.insert(ordinal).second) return gap(TemplateGapKind::Observation, "input_message_native_group_not_qualified");
        const auto dependencies = metadata.find("completion_entry_dependencies");
        if (dependencies == metadata.end()) return gap(TemplateGapKind::Observation, "host_message_completion_dependencies_not_observed");
        if (dependencies->is_null()) return gap(TemplateGapKind::Rule, "host_message_completion_protocol_not_supported");
        const auto message_rule = HostMessageRule::bind(root, ranks, *dependencies);
        if (!message_rule) return gap(TemplateGapKind::Rule, std::string(message_rule.error()));

        const TemplateRegion * submission = nullptr;
        const TemplateRegion * wait = nullptr;
        for (const auto & child : definition_.regions) {
            if (child.parent_call != region.source_call) continue;
            const bool submitting = child.semantic == "input_message_submit";
            if (!submitting && child.semantic != "input_message_wait") continue;
            auto & slot = submitting ? submission : wait;
            if (slot || !child.cpu_scope || records.at(calls[child.source_call].identity).at("metadata").at("message") != metadata)
                return gap(TemplateGapKind::Observation, "input_message_native_completion_not_unique");
            slot = &child;
        }
        if (!submission) return gap(TemplateGapKind::Observation, "input_message_submission_not_observed");
        const bool returns_work = native_regions_.at(submission->source_call).result.at("returns_work").get<bool>();
        if (returns_work != (wait != nullptr)) return gap(TemplateGapKind::Observation, "input_message_work_wait_not_observed");

        // The returned Work identifies its actual blocking wait. Enqueue CPU
        // remains ordinary work; its return does not timestamp transport start.
        // A synchronous backend without Work owns its whole native submission.
        const auto & completion = returns_work ? *wait : *submission;
        TemplateCollectiveInterface binding{ channel + "/" + std::to_string(ordinal),
                                             CommunicationOperation::Broadcast,
                                             std::nullopt,
                                             metadata.at("data_type").get<std::string>(),
                                             logical_tp_size,
                                             *region.cpu_scope,
                                             index,
                                             definition_.graph.boundary(submission->cpu_scope->begin).lane,
                                             { submission->cpu_scope->begin },
                                             { completion.cpu_scope->end },
                                             std::nullopt,
                                             std::nullopt };
        binding.host_message = *message_rule;
        for (const auto node : completion.nodes) {
            const auto & work = definition_.graph[node];
            if (work.kind == SourceSpanKind::CpuControl || work.kind == SourceSpanKind::HostWait) binding.substitute_nodes.push_back(node);
            else if (work.role() != TemplateNodeRole::Annotation) return gap(TemplateGapKind::Rule, "input_message_completion_contains_other_service");
        }
        definition_.collective_interfaces.push_back(std::move(binding));
    }
    if (ordinals.empty()) return gap(TemplateGapKind::Observation, "input_message_calls_not_observed");
    for (std::uint32_t ordinal = 0; ordinal < ordinals.size(); ++ordinal)
        if (!ordinals.contains(ordinal)) return gap(TemplateGapKind::Observation, "input_message_framing_order_not_observed");
    return {};
}

std::expected<void, TemplateGap> SGLangTemplateBindings::bind_collective_interfaces(std::uint32_t logical_tp_size, std::span<const SourceCall> source_calls,
                                                                                    const nlohmann::json & facts) {
    if (const auto messages = bind_input_messages(logical_tp_size, source_calls, facts.at("calls")); !messages) return messages;
    std::map<std::size_t, std::size_t> regions;
    std::vector<std::size_t> calls;
    std::set<std::size_t> model_lanes;
    DependencyPaths<std::size_t> paths;
    for (std::size_t index = 0; index < definition_.regions.size(); ++index) {
        const auto & region = definition_.regions[index];
        regions.emplace(region.source_call, index);
        if (region.semantic == "logical_collective") {
            if (!region.cpu_scope) return gap(TemplateGapKind::Observation, "collective_host_scope_not_observed");
            calls.push_back(index);
        }
    }
    if (calls.empty()) {
        if (logical_tp_size > 1 && (definition_.step == StepKind::Prefill || definition_.step == StepKind::Decode))
            return gap(TemplateGapKind::Observation, "parallel_calls_not_observed");
        return {};
    }
    for (const auto & node : definition_.graph) {
        paths.connect(node.begin, node.end);
        const auto lane = definition_.graph.boundary(node.begin).lane;
        if (definition_.lanes[lane].role == TemplateResourceRole::Model) model_lanes.insert(lane);
    }
    for (const auto & edge : definition_.graph.dependencies()) paths.connect(edge.source, edge.target);
    const auto reverse = paths.reversed();
    if (model_lanes.size() != 1) return gap(model_lanes.empty() ? TemplateGapKind::Observation : TemplateGapKind::Rule, "collective_model_stream_not_unique");
    const auto model_lane = *model_lanes.begin();

    // Semantic order is local CPU call order. It never joins clocks or source
    // span IDs from separate ranks; the compiled position names do that job.
    std::ranges::sort(calls, {}, [&](std::size_t index) {
        return definition_.boundary_observations[definition_.regions[index].cpu_scope->begin].observed_time_ns;
    });
    std::set<std::pair<std::string, std::optional<std::uint32_t>>> positions;
    for (const auto index : calls) {
        const auto & region = definition_.regions[index];
        const auto & native = native_regions_.at(region.source_call);
        const auto & parent = native_regions_.at(region.parent_call);
        const auto & input = native.inputs.at("input_");
        const auto & output = native.outputs.at("output");
        const auto size = native.parameters.at("logical_tp_size");
        if (!input || !output) return gap(TemplateGapKind::Observation, "collective_local_layout_not_qualified");
        if (input->shape.size() != 2 || input->dtype != output->dtype || native.parameters.at("physical_tp_size") != 1 || size != logical_tp_size
            || native.parameters.at("logical_rank") != definition_.rank || size < 2)
            return gap(TemplateGapKind::Rule, "collective_local_layout_not_qualified");

        const bool gather = native.method == "all_gather";
        const auto operation = gather ? CommunicationOperation::AllGather : CommunicationOperation::AllReduce;
        const auto rows = gather ? CollectiveRows::Requests : CollectiveRows::Tokens;
        const CollectiveCountRule count{ rows, static_cast<std::uint64_t>(input->shape[1]) };
        std::uint64_t observed_rows = definition_.source_requests.size();
        if (rows == CollectiveRows::Tokens) {
            observed_rows = 0;
            for (const auto & request : definition_.source_requests) observed_rows += request.input_tokens;
        }
        if (input->shape[0] < 1 || input->shape[1] < 1 || observed_rows != static_cast<std::uint64_t>(input->shape[0]))
            return gap(TemplateGapKind::Rule, "collective_input_relation_not_qualified");
        auto shape = input->shape;
        if (gather) {
            if (parent.semantic != "logits" || native.call_parameters.at("dim") != -1)
                return gap(TemplateGapKind::Rule, "collective_gather_layout_not_qualified");
            shape.back() *= size;
        }
        else if (parent.semantic != "embedding" && parent.semantic != "decoder_layer" && parent.semantic != "feed_forward")
            return gap(TemplateGapKind::Rule, "collective_reduce_layout_not_qualified");
        if (output->shape != shape) return gap(TemplateGapKind::Rule, "collective_output_relation_not_qualified");
        if (input->stride != std::vector<std::int64_t>{ input->shape[1], 1 } || output->stride != std::vector<std::int64_t>{ output->shape[1], 1 })
            return gap(TemplateGapKind::Rule, "collective_contiguous_layout_not_qualified");

        TemplateCollectiveInterface binding{ parent.semantic + "/" + native.method,
                                             operation,
                                             count,
                                             input->dtype,
                                             static_cast<std::uint32_t>(size),
                                             *region.cpu_scope,
                                             index,
                                             model_lane,
                                             {},
                                             {},
                                             std::nullopt,
                                             std::nullopt };
        auto ancestor = region.parent_call;
        while (ancestor != definition_.source_call) {
            const auto enclosing = regions.at(ancestor);
            const auto & scope = definition_.regions[enclosing];
            if (scope.semantic == "decoder_layer") {
                binding.model_region = enclosing;
                binding.model_layer = static_cast<std::uint32_t>(native_regions_.at(ancestor).parameters.at("layer_index"));
                break;
            }
            ancestor = scope.parent_call;
        }
        if (!positions.emplace(binding.position, binding.model_layer).second) return gap(TemplateGapKind::Rule, "collective_semantic_position_not_unique");
        const auto owns = [&](std::optional<std::size_t> scope) {
            while (scope && *scope != definition_.source_call) {
                if (*scope == region.source_call) return true;
                const auto found = regions.find(*scope);
                if (found == regions.end()) return false;
                scope = definition_.regions[found->second].parent_call;
            }
            return false;
        };
        std::vector<const SourceCollective *> envelopes;
        for (const auto & [identity, collective] : source_.collectives())
            if (owns(source_.spans()[identity].scope_call)) envelopes.push_back(&collective);
        if (envelopes.size() != 1)
            return gap(envelopes.empty() ? TemplateGapKind::Observation : TemplateGapKind::Rule, "collective_transport_envelope_not_unique");
        const auto & envelope = *envelopes.front();
        if (envelope.rank != definition_.rank || envelope.count != static_cast<std::uint64_t>(input->shape[0]) * static_cast<std::uint64_t>(input->shape[1])
            || input->dtype != "torch.bfloat16" || envelope.data_type != "BFP16")
            return gap(TemplateGapKind::Rule, "collective_transport_work_not_qualified");
        std::map<BoundaryId, std::size_t> local_points;
        for (std::size_t point = 0; point < definition_.boundary_observations.size(); ++point)
            local_points.emplace(definition_.boundary_observations[point].source_boundary, point);
        std::map<SourceSpanId, std::size_t> local_nodes;
        for (std::size_t node = 0; node < definition_.graph.size(); ++node)
            local_nodes.emplace(definition_.observation(definition_.graph[node]).source_span, node);
        std::set<std::size_t> transport, removed;
        for (const auto & task : envelope.tasks) {
            const auto found = local_nodes.find(task.span);
            if (found == local_nodes.end()) return gap(TemplateGapKind::Observation, "collective_transport_task_not_owned");
            const auto & node = definition_.graph[found->second];
            if (definition_.lanes[definition_.graph.boundary(node.begin).lane].source.domain != SourceDomain::DeviceStream)
                return gap(TemplateGapKind::Observation, "collective_transport_task_not_device");
            removed.insert(found->second);
            transport.insert(node.begin);
            transport.insert(node.end);
        }
        // Auxiliary work is identified by the raw observer propagation, not by
        // its kernel or by membership of the surrounding logical call.
        for (const auto node : region.nodes) {
            const auto & work = definition_.graph[node];
            const auto & observed = source_.spans()[definition_.observation(work).source_span];
            if (definition_.lanes[definition_.graph.boundary(work.begin).lane].source.domain == SourceDomain::DeviceStream && observed.source_interval
                && facts.at("intervals").at(*observed.source_interval).value("observer", false))
                removed.insert(node);
        }
        std::map<std::size_t, std::vector<std::size_t>> joins;
        std::set<std::size_t> mixed;
        for (const auto & edge : definition_.graph.dependencies()) {
            if (edge.relation == SourceRelation::CompletionJoin
                && definition_.lanes[definition_.graph.boundary(edge.target).lane].source.domain == SourceDomain::DeviceTaskCompletion)
                joins[edge.target].push_back(edge.source);
            else mixed.insert(edge.target);
        }
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto & [point, contributors] : joins)
                if (!transport.contains(point) && !mixed.contains(point) && !contributors.empty()
                    && std::ranges::all_of(contributors, [&](auto source) { return transport.contains(source); })) {
                    transport.insert(point);
                    changed = true;
                }
        }
        std::set<std::size_t> inputs, launches, outputs;
        if (transport.empty()) {
            // An empty PG1 device operation still has a native CPU submission.
            // Locate its slot only between actual submissions on the unique
            // qualified collective stream and the same native CPU queue.
            const auto & observed = source_.spans()[envelope.span];
            std::set<std::size_t> anchors;
            for (const auto & edge : source_.edges())
                if (edge.relation == SourceRelation::NativeFlow && edge.target == observed.begin
                    && source_.lanes()[source_.boundaries()[edge.source].lane].domain == SourceDomain::CpuThread)
                    if (const auto found = local_points.find(edge.source); found != local_points.end()) anchors.insert(found->second);
            if (anchors.size() != 1) return gap(TemplateGapKind::Observation, "collective_empty_transport_launch_not_bound");
            const auto anchor = *anchors.begin();
            const auto cpu_lane = definition_.graph.boundary(anchor).lane;
            const auto preceding = reverse.reachable({ anchor });
            const auto following = paths.reachable({ anchor });
            std::set<std::size_t> lanes;
            for (std::size_t lane = 0; lane < definition_.lanes.size(); ++lane)
                if (definition_.lanes[lane].role == TemplateResourceRole::Collective) lanes.insert(lane);
            if (lanes.size() != 1)
                return gap(lanes.empty() ? TemplateGapKind::Observation : TemplateGapKind::Rule, "collective_empty_transport_stream_not_unique");
            const auto lane = *lanes.begin();
            std::set<std::size_t> before, after;
            for (const auto & edge : definition_.graph.dependencies()) {
                if (edge.relation != SourceRelation::NativeFlow || definition_.graph.boundary(edge.source).lane != cpu_lane
                    || definition_.graph.boundary(edge.target).lane != lane)
                    continue;
                const auto node = std::ranges::find_if(region.nodes, [&](auto index) { return definition_.graph[index].begin == edge.target; });
                if (node == region.nodes.end() || removed.contains(*node)) continue;
                if (edge.source == anchor) continue;
                if (preceding.contains(edge.source)) before.insert(definition_.graph[*node].end);
                if (following.contains(edge.source)) after.insert(edge.target);
            }
            before = reverse.frontier(std::move(before));
            after = paths.frontier(std::move(after));
            if (before.size() != 1 || after.size() != 1 || !following.contains(*after.begin()))
                return gap(TemplateGapKind::Observation, "collective_empty_transport_stream_seam_not_bound");
            // The original task-order edge can pass through a pure completion
            // join. No CPU/device clock subtraction participates in this cut.
            std::set<std::size_t> predecessor{ *before.begin() };
            for (const auto & [point, contributors] : joins)
                if (!mixed.contains(point) && !contributors.empty()
                    && std::ranges::all_of(contributors, [&](auto source) { return predecessor.contains(source); }))
                    predecessor.insert(point);
            for (std::size_t edge = 0; edge < definition_.graph.dependencies().size(); ++edge) {
                const auto & dependency = definition_.graph.dependencies()[edge];
                if ((dependency.relation == SourceRelation::NativeTaskDependency || dependency.relation == SourceRelation::StreamOrder)
                    && predecessor.contains(dependency.source) && dependency.target == *after.begin())
                    binding.seam_dependencies.push_back(edge);
            }
            if (binding.seam_dependencies.size() != 1)
                return gap(binding.seam_dependencies.empty() ? TemplateGapKind::Observation : TemplateGapKind::Rule,
                           "collective_empty_transport_order_not_bound");
            const auto & seam = definition_.graph.dependencies()[binding.seam_dependencies.front()];
            inputs.insert(seam.source);
            launches.insert(anchor);
            outputs.insert(seam.target);
        }
        else {
            for (const auto & edge : definition_.graph.dependencies()) {
                const bool from = transport.contains(edge.source), to = transport.contains(edge.target);
                if (!from && to) {
                    const auto domain = definition_.lanes[definition_.graph.boundary(edge.source).lane].source.domain;
                    (domain == SourceDomain::CpuThread ? launches : inputs).insert(edge.source);
                }
                if (from && !to) outputs.insert(edge.target);
            }
            if (inputs.empty() || outputs.empty()) return gap(TemplateGapKind::Observation, "collective_transport_stream_seam_not_bound");
            // A local scalar transport completion may replace an outgoing cut
            // only when every actual transport tail already precedes it.
            std::set<std::size_t> ends;
            for (const auto & task : envelope.tasks) ends.insert(local_points.at(source_.spans()[task.span].end));
            ends = reverse.frontier(std::move(ends));
            for (const auto tail : ends) {
                const auto reached = paths.reachable({ tail });
                if (!std::ranges::all_of(outputs, [&](auto point) { return reached.contains(point); }))
                    return gap(TemplateGapKind::Rule, "collective_transport_exit_not_closed");
            }
            for (std::size_t connection = 0; connection < definition_.graph.connection_count(); ++connection)
                if (transport.contains(definition_.connection(connection).local_boundary))
                    return gap(TemplateGapKind::Rule, "collective_transport_external_connection_not_bound");
        }
        binding.launch_inputs.assign(launches.begin(), launches.end());
        binding.substitute_nodes.assign(removed.begin(), removed.end());
        binding.transport_boundaries.assign(transport.begin(), transport.end());
        definition_.collective_interfaces.push_back(std::move(binding));
    }
    return {};
}

} // namespace markov
