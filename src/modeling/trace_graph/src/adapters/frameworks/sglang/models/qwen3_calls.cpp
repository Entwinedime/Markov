#include "markov/trace_graph/adapters/frameworks/sglang/source_calls.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include <variant>

namespace markov::trace_graph::adapters::sglang {

DenseCallAudit observe_dense_calls(const source_dag::SourceDagIndex & source, const simulation::detail::CpuTaskQueues & queues,
                                   const PhaseCallPositions & positions, const AttentionCallAudit & attention) {
    DenseCallAudit result;
    using Identity = std::tuple<size_t, std::optional<uint64_t>, uint64_t>;
    std::map<Identity, std::vector<size_t>> attention_by_layer;
    const auto & graph = source.graph();
    std::set<uint64_t> qwen3_layers, start_layers, output_norm_layers, logits_layers;
    std::map<uint64_t, uint64_t> input_embedding_layers;
    for (const auto & event : graph.runtime_observations()) {
        if ((event.name == "runtime.phase.linear" || event.name == "runtime.phase.gated_activation" || event.name == "runtime.phase.normalization"
             || event.name == "runtime.phase.qkv_norm_rope")
            && event.arg("model_layer_implementation") == "sglang.srt.models.qwen3.Qwen3DecoderLayer" && event.has_arg("model_layer_index")) {
            qwen3_layers.insert(event.arg_u64("model_layer_index"));
            if (event.has_arg("model_start_layer_index")) start_layers.insert(event.arg_u64("model_start_layer_index"));
            if (event.arg("model_output_norm_present") == "true") output_norm_layers.insert(event.arg_u64("model_layer_index"));
            if (event.arg("model_logits_present") == "true") logits_layers.insert(event.arg_u64("model_layer_index"));
            if (event.arg("model_input_embedding_present") == "true")
                input_embedding_layers.emplace(event.arg_u64("model_layer_index"), event.arg_u64("model_input_embedding_tp_size"));
        }
    }
    for (size_t i = 0; i < attention.calls.size(); ++i) {
        const auto & call = attention.calls[i];
        const auto & location = call.region.descriptor.value();
        const auto & descriptor =
            location.prelude ? graph.prelude_context_events().at(location.observation) : graph.runtime_observations().at(location.observation);
        if (!descriptor.has_arg("layer_id")) continue;
        attention_by_layer[{ call.position.phase_index, call.position.decode_iteration, descriptor.arg_u64("layer_id") }].push_back(i);
    }

    for (auto & region : observe_dense_regions(source, queues, result.issues)) {
        const auto & descriptor = graph.runtime_observations().at(region.descriptor_observation.value_or(region.work.observation));
        if (descriptor.arg("model_layer_implementation") != "sglang.srt.models.qwen3.Qwen3DecoderLayer") continue;
        const bool embedding_output_mask = std::holds_alternative<structure::EmbeddingOutputMaskShape>(region.shape);
        const auto role = embedding_output_mask ? "input_embedding_output_mask" : descriptor.arg("dense_role");
        const bool gate = std::holds_alternative<structure::GatedActivationShape>(region.shape);
        const bool norm = std::holds_alternative<structure::NormalizationShape>(region.shape);
        const bool qkv = std::holds_alternative<structure::QkvRopeShape>(region.shape);
        const bool table = std::holds_alternative<structure::RotaryTableShape>(region.shape);
        const bool rotary = std::holds_alternative<structure::RotaryShape>(region.shape);
        const bool reshape = std::holds_alternative<structure::QkReshapeShape>(region.shape);
        const bool embedding = std::holds_alternative<structure::EmbeddingShape>(region.shape);
        const bool embedding_mask = std::holds_alternative<structure::EmbeddingMaskShape>(region.shape);
        const bool logits_output = std::holds_alternative<structure::LogitsOutputShape>(region.shape);
        const bool logits_pruning = std::holds_alternative<structure::LogitsPruningShape>(region.shape);
        const bool input_buffers = std::holds_alternative<structure::InputBuffersShape>(region.shape);
        const bool sampling = std::holds_alternative<structure::GreedySamplingShape>(region.shape);
        if (gate && role != "mlp_activation") continue;
        if (norm && role != "input_norm" && role != "post_attention_norm" && role != "query_norm" && role != "key_norm" && role != "output_norm") continue;
        if (role == "output_norm" && (!norm || descriptor.arg("model_implementation") != "sglang.srt.models.qwen3.Qwen3Model")) continue;
        if (role == "logits_projection" && (!region.implementation.starts_with("torch.matmul.logits:") || descriptor.arg("last_token_projection") != "true"))
            continue;
        if (qkv && role != "qkv_norm_rope") continue;
        if (table && role != "rotary_table") continue;
        if (rotary && role != "rotary") continue;
        if (reshape && role != "query_reshape" && role != "key_reshape") continue;
        if (embedding && role != "input_embedding") continue;
        if (embedding_mask && role != "input_embedding_mask") continue;
        if (logits_output && role != "logits_output") continue;
        if (logits_pruning && role != "logits_pruning") continue;
        if (input_buffers && role != "input_buffers") continue;
        if (sampling && role != "greedy_sampling") continue;
        if (!gate && !norm && !qkv && !table && !rotary && !reshape && !embedding && !embedding_mask && !embedding_output_mask && !logits_output
            && !logits_pruning && !input_buffers && !sampling && role != "qkv_projection" && role != "attention_output" && role != "mlp_gate_up"
            && role != "mlp_down" && role != "logits_projection")
            continue;
        const auto owner = call_phase_position(region.work, positions);
        if (!owner || !descriptor.has_arg("model_layer_index")) {
            ++result.issues["dense_call_phase_or_layer_unavailable"];
            continue;
        }

        const auto layer = descriptor.arg_u64("model_layer_index");
        if (table && (!owner->decode_iteration || !descriptor.has_arg("model_start_layer_index") || descriptor.arg_u64("model_start_layer_index") != layer)) {
            ++result.issues["rotary_table_shared_owner_unresolved"];
            continue;
        }
        const bool head_norm = role == "query_norm" || role == "key_norm";
        const auto rows_per_token = descriptor.arg_u64("rows_per_token");
        if (rows_per_token == 0 || (!head_norm && rows_per_token != 1)) {
            ++result.issues["dense_token_row_mapping_unavailable"];
            continue;
        }
        const auto match = attention_by_layer.find({ owner->phase_index, owner->decode_iteration, layer });
        if (match == attention_by_layer.end() || match->second.size() != 1) {
            ++result.issues["dense_call_token_layout_unresolved"];
            continue;
        }
        result.calls.push_back({ std::move(region), *owner, match->second.front(), role, layer, rows_per_token });
    }
    std::map<std::pair<Identity, std::string>, size_t> counts;
    std::set<size_t> sampling_descriptors;
    for (const auto & call : result.calls) {
        const auto count = ++counts[{
            { call.position.phase_index, call.position.decode_iteration, call.layer_index },
            call.role
        }];
        if (call.role == "greedy_sampling") {
            sampling_descriptors.insert(call.region.descriptor_observation.value());
            if (count > 1) ++result.issues["greedy_sampling_correspondence_ambiguous"];
        }
    }
    // A declared greedy call that cannot identify its inner argmax is a
    // coverage gap, not permission to treat unknown work as cache-independent.
    // Do not require a sampler in phases that never declared one (e.g. scoring).
    for (size_t i = 0; i < graph.runtime_observations().size(); ++i) {
        const auto & event = graph.runtime_observations()[i];
        if (event.name == "runtime.phase.greedy_sampling" && event.arg("model_layer_implementation") == "sglang.srt.models.qwen3.Qwen3DecoderLayer"
            && event.arg("formal_window_context") != "causal_head" && !sampling_descriptors.contains(i))
            ++result.issues["greedy_sampling_without_complete_call"];
    }
    for (const auto & [identity, calls] : attention_by_layer) {
        if (!qwen3_layers.contains(std::get<2>(identity))) continue;
        if (logits_layers.contains(std::get<2>(identity))) {
            for (const auto * role : { "logits_projection", "logits_output" }) {
                const auto count = counts[{ identity, role }];
                if (count == 0) ++result.issues[std::string(role) + "_without_complete_call"];
                else if (count > 1) ++result.issues[std::string(role) + "_correspondence_ambiguous"];
            }
        }
        if (const auto embedding = input_embedding_layers.find(std::get<2>(identity)); embedding != input_embedding_layers.end()) {
            for (const auto * role : { "input_embedding", "input_embedding_mask", "input_embedding_output_mask" }) {
                if (embedding->second == 1 && std::string_view(role) != "input_embedding") continue;

                const auto count = counts[{ identity, role }];
                if (count == 0) ++result.issues[std::string(role) + "_without_complete_call"];
                else if (count > 1) ++result.issues[std::string(role) + "_correspondence_ambiguous"];
            }
        }
        if (output_norm_layers.contains(std::get<2>(identity))) {
            const auto count = counts[{ identity, "output_norm" }];
            if (count == 0) ++result.issues["output_norm_without_complete_call"];
            else if (count > 1) ++result.issues["output_norm_correspondence_ambiguous"];
        }
        if (!std::get<1>(identity)) {
            for (const auto * role : { "rotary", "query_reshape", "key_reshape" }) {
                const auto count = counts[{ identity, role }];
                if (count == 0) ++result.issues[std::string(role) + "_without_complete_call"];
                else if (count > 1) ++result.issues[std::string(role) + "_correspondence_ambiguous"];
            }
        }
        if (std::get<1>(identity) && start_layers.contains(std::get<2>(identity))) {
            const auto count = counts[{ identity, "rotary_table" }];
            if (count == 0) ++result.issues["rotary_table_without_complete_call"];
            else if (count > 1) ++result.issues["rotary_table_correspondence_ambiguous"];
        }
        for (const auto * role : { "qkv_projection",
                                   "attention_output",
                                   "mlp_gate_up",
                                   "mlp_down",
                                   "mlp_activation",
                                   "input_norm",
                                   "post_attention_norm",
                                   "query_norm",
                                   "key_norm",
                                   "qkv_norm_rope" }) {
            // NPU Decode fuses Q/K normalization with split and rotary
            // encoding; it must not require or generate separate RMS calls.
            if (std::get<1>(identity) && (std::string_view(role) == "query_norm" || std::string_view(role) == "key_norm")) continue;
            if (!std::get<1>(identity) && std::string_view(role) == "qkv_norm_rope") continue;
            const auto count = counts[{ identity, role }];
            if (count == 0) ++result.issues["dense_operation_without_complete_call"];
            else if (count > 1) ++result.issues["dense_operation_correspondence_ambiguous"];
        }
    }
    return result;
}

} // namespace markov::trace_graph::adapters::sglang
