#include "markov/adapters/frameworks/sglang/models/qwen3_regions.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <string_view>

namespace markov {

bool Qwen3Regions::supports(const nlohmann::json & configuration) { return configuration.at("architectures") == nlohmann::json::array({ "Qwen3ForCausalLM" }); }

std::span<const NativeRegionBinding> Qwen3Regions::bindings() {
    using namespace std::string_view_literals;
    static constexpr std::array model_inputs{ "input_ids"sv, "positions"sv, "input_embeds"sv };
    static constexpr std::array embedding_inputs{ "input_"sv };
    static constexpr std::array layer_inputs{ "positions"sv, "hidden_states"sv, "residual"sv };
    static constexpr std::array attention_inputs{ "positions"sv, "hidden_states"sv };
    static constexpr std::array radix_inputs{ "q"sv, "k"sv, "v"sv };
    static constexpr std::array mlp_inputs{ "x"sv };
    static constexpr std::array logits_inputs{ "input_ids"sv, "hidden_states"sv };
    static constexpr std::array selection_inputs{ "hidden_states"sv };
    static constexpr std::array qkv_outputs{
        NativeRegionOutput{ "q", 0 },
        NativeRegionOutput{ "k", 1 },
        NativeRegionOutput{ "v", 2 }
    };
    static constexpr std::array qkv_module_inputs{
        NativeRegionParameter{ "q_norm_weight", "q_norm.weight" },
        NativeRegionParameter{ "k_norm_weight", "k_norm.weight" }
    };
    static constexpr std::array selection_outputs{
        NativeRegionOutput{ "pruned_states", 0 }
    };
    static constexpr std::array selection_parameters{
        NativeRegionParameter{          "forward_mode",          "logits_metadata.forward_mode" },
        NativeRegionParameter{ "extend_return_logprob", "logits_metadata.extend_return_logprob" },
        NativeRegionParameter{     "padded_static_len",     "logits_metadata.padded_static_len" }
    };

    static constexpr std::array model_parameters{
        NativeRegionParameter{ "start_layer", "start_layer" },
        NativeRegionParameter{   "end_layer",   "end_layer" }
    };
    static constexpr std::array embedding_parameters{
        NativeRegionParameter{     "hidden_size",  "embedding_dim" },
        NativeRegionParameter{ "vocabulary_size", "num_embeddings" }
    };
    static constexpr std::array layer_parameters{
        NativeRegionParameter{ "layer_index", "self_attn.attn.layer_id" }
    };
    static constexpr std::array attention_parameters{
        NativeRegionParameter{ "layer_index", "attn.layer_id" },
        NativeRegionParameter{ "hidden_size",   "hidden_size" },
        NativeRegionParameter{ "query_heads",     "num_heads" },
        NativeRegionParameter{    "kv_heads",  "num_kv_heads" },
        NativeRegionParameter{    "head_dim",      "head_dim" }
    };
    static constexpr std::array radix_parameters{
        NativeRegionParameter{ "layer_index",      "layer_id" },
        NativeRegionParameter{ "query_heads", "tp_q_head_num" },
        NativeRegionParameter{    "kv_heads", "tp_k_head_num" },
        NativeRegionParameter{    "head_dim",      "head_dim" }
    };
    static constexpr std::array mlp_parameters{
        NativeRegionParameter{            "hidden_size",            "gate_up_proj.input_size" },
        NativeRegionParameter{      "intermediate_size",               "down_proj.input_size" },
        NativeRegionParameter{ "intermediate_partition", "down_proj.input_size_per_partition" }
    };
    static constexpr std::array values{
        NativeRegionBinding{ .identity = "sglang.qwen3.model",
                            .semantic = "model_body",
                            .module_type = "sglang.srt.models.qwen3.Qwen3Model",
                            .method = "forward",
                            .inputs = model_inputs,
                            .parameters = model_parameters },
        NativeRegionBinding{ .identity = "sglang.qwen3.embedding",
                            .semantic = "embedding",
                            .module_type = "sglang.srt.layers.vocab_parallel_embedding.VocabParallelEmbedding",
                            .method = "forward",
                            .inputs = embedding_inputs,
                            .parameters = embedding_parameters },
        NativeRegionBinding{ .identity = "sglang.qwen3.layer",
                            .semantic = "decoder_layer",
                            .module_type = "sglang.srt.models.qwen3.Qwen3DecoderLayer",
                            .method = "forward",
                            .inputs = layer_inputs,
                            .parameters = layer_parameters },
        NativeRegionBinding{ .identity = "sglang.qwen3.attention",
                            .semantic = "attention",
                            .module_type = "sglang.srt.models.qwen3.Qwen3Attention",
                            .method = "forward",
                            .inputs = attention_inputs,
                            .parameters = attention_parameters },
        NativeRegionBinding{ .identity = "sglang.qwen3.qkv.native",
                            .semantic = "qkv_preparation",
                            .module_type = "sglang.srt.models.qwen3.Qwen3Attention",
                            .method = "forward_prepare_native",
                            .inputs = attention_inputs,
                            .parameters = attention_parameters,
                            .outputs = qkv_outputs,
                            .module_inputs = qkv_module_inputs },
        NativeRegionBinding{ .identity = "sglang.qwen3.qkv.npu",
                            .semantic = "qkv_preparation",
                            .module_type = "sglang.srt.models.qwen3.Qwen3Attention",
                            .method = "forward_prepare_npu",
                            .inputs = attention_inputs,
                            .parameters = attention_parameters,
                            .outputs = qkv_outputs,
                            .module_inputs = qkv_module_inputs },
        NativeRegionBinding{ .identity = "sglang.qwen3.radix_attention",
                            .semantic = "attention_core",
                            .module_type = "sglang.srt.layers.radix_attention.RadixAttention",
                            .method = "forward",
                            .inputs = radix_inputs,
                            .parameters = radix_parameters },
        NativeRegionBinding{ .identity = "sglang.qwen3.mlp",
                            .semantic = "feed_forward",
                            .module_type = "sglang.srt.models.qwen2.Qwen2MLP",
                            .method = "forward",
                            .inputs = mlp_inputs,
                            .parameters = mlp_parameters },
        NativeRegionBinding{ .identity = "sglang.qwen3.logits",
                            .semantic = "logits",
                            .module_type = "sglang.srt.layers.logits_processor.LogitsProcessor",
                            .method = "forward",
                            .inputs = logits_inputs,
                            .parameters = {} },
        NativeRegionBinding{ .identity = "sglang.qwen3.logits.selection",
                            .semantic = "logits_selection",
                            .module_type = "sglang.srt.layers.logits_processor.LogitsProcessor",
                            .method = "_get_pruned_states",
                            .inputs = selection_inputs,
                            .parameters = {},
                            .call_parameters = selection_parameters,
                            .outputs = selection_outputs },
    };

    return values;
}

} // namespace markov
