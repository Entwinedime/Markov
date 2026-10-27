#include "markov/adapters/sglang/observations.hpp"

#include <array>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace markov::adapters::sglang {

using namespace trace_graph::source;
namespace {

using namespace trace_graph::planning;

struct ComputeObservation {
    ComputeOperation operation;
    ComputeInput input;
    std::optional<RowBinding> rows;
};

std::vector<ComputeObservation> read_compute(const ProbeEvent & event, const ComputeFrame & frame, const AttentionDescriptor * descriptor) {
    const auto & args = event.args;
    if (event.name == "runtime.phase.attention") {
        if (!descriptor->operation || frame.sequence_tokens.size() != 1 || !frame.query_tokens || frame.query_tokens->size() != 1) return {};
        std::vector<ComputeObservation> result;
        for (const auto & write : descriptor->cache_writes) {
            const auto rows =
                descriptor->tensor_query_tokens == frame.query_tokens->front() ? std::optional(RowBinding{ RowBasis::QueryTokens, 1 }) : std::nullopt;
            result.push_back({ write, RowInput{ descriptor->tensor_query_tokens }, rows });
        }
        result.push_back({
            *descriptor->operation,
            AttentionInput{ frame.query_tokens->front(), descriptor->tensor_query_tokens, frame.sequence_tokens.front(), 1, descriptor->page_tokens },
            std::nullopt
        });
        return result;
    }

    const auto input = args.at("input_shape").get<std::vector<double>>();
    const auto stride = args.at("input_stride").get<std::vector<double>>();
    if (input.size() != 2 || stride.size() != 2) return {};
    const std::array<double, 2> input_stride{ stride[0], stride[1] };
    const auto basis =
        event.name == "runtime.phase.logits_projection" && args.at("last_token_projection").get<bool>() ? RowBasis::BatchSize : RowBasis::QueryTokens;
    std::optional<RowBinding> rows;
    if (args.contains("rows_per_token") && !args.at("rows_per_token").is_null()) rows = RowBinding{ basis, args.at("rows_per_token").get<double>() };

    if (event.name == "runtime.phase.linear" || event.name == "runtime.phase.logits_projection") {
        const auto weight = args.at("weight_shape").get<std::vector<double>>();
        const auto weight_stride = args.at("weight_stride").get<std::vector<double>>();
        if (weight.size() != 2 || weight_stride.size() != 2) return {};

        const auto method = event.name == "runtime.phase.logits_projection" ? "torch.matmul.logits:" : "torch.linear:";
        auto implementation = method + args.at("input_dtype").get<std::string>();
        implementation += args.at("bias_shape").is_null() ? ":no_bias" : ":bias";
        if (args.at("input_dtype") != args.at("weight_dtype")) implementation += ":weight=" + args.at("weight_dtype").get<std::string>();
        return {
            ComputeObservation{ { std::move(implementation), LinearGeometry{ weight[0], input[1], input_stride, { weight_stride[0], weight_stride[1] } } },
                               RowInput{ input[0] },
                               rows }
        };
    }
    if (event.name == "runtime.phase.normalization") {
        const auto implementation = "torch_npu.rms_norm:" + args.at("input_dtype").get<std::string>() + ":weight=" + args.at("weight_dtype").get<std::string>();
        const ComputeObservation normalization{
            { implementation, NormalizationGeometry{ input[1], args.at("epsilon").get<double>(), input_stride } },
            RowInput{ input[0] },
            rows
        };
        if (args.at("implementation") == "torch_npu.npu_rms_norm") return { normalization };
        if (args.at("implementation") != "torch_npu.npu_add_rms_norm" || !args.at("split_residual_norm").get<bool>()
            || args.at("post_residual_addition").get<bool>() || args.at("input_shape") != args.at("residual_shape")
            || args.at("input_stride") != args.at("residual_stride") || args.at("input_dtype") != args.at("residual_dtype") || stride[0] != input[1]
            || stride[1] != 1)
            return {};

        // The installed batch-invariant backend executes an add then RMSNorm.
        // Preserve both operations; the enclosing probe wall is not charged.
        return {
            ComputeObservation{ { "aten.add:" + args.at("input_dtype").get<std::string>(), AdditionGeometry{ input[1], input_stride } },
                               RowInput{ input[0] },
                               rows },
            normalization
        };
    }
    if (event.name == "runtime.phase.gated_activation" && args.at("implementation") == "torch_npu.npu_swiglu") {
        const auto output = args.at("output_shape").get<std::vector<double>>();
        if (output.size() != 2) return {};
        return {
            ComputeObservation{ { "torch_npu.swiglu:" + args.at("input_dtype").get<std::string>(), ActivationGeometry{ output[1], input_stride } },
                               RowInput{ input[0] },
                               rows }
        };
    }
    return {};
}

std::optional<std::vector<ComputeOperation>> attention_writes(const nlohmann::json & args) {
    if (!args.at("save_kv_cache").get<bool>() || !args.at("key_present").get<bool>() || !args.at("value_present").get<bool>())
        return std::vector<ComputeOperation>{};

    const auto & cache = args.at("cache_write");
    if (cache.at("pool_implementation") != "sglang.srt.hardware_backend.npu.memory_pool_npu.NPUMHATokenToKVPool" || cache.at("use_fia") != true
        || cache.at("cache_dtype") != "torch.bfloat16" || cache.at("key_dtype") != cache.at("cache_dtype") || cache.at("value_dtype") != cache.at("cache_dtype")
        || cache.at("location_dtype") != "torch.int64")
        return std::nullopt;

    std::vector<ComputeOperation> result;
    for (const auto * tensor : { "key", "value" }) {
        const auto shape = cache.at(std::string(tensor) + "_shape").get<std::vector<double>>();
        const auto stride = cache.at(std::string(tensor) + "_stride").get<std::vector<double>>();
        if (shape.size() != 3 || stride.size() != 3 || stride[2] != 1 || stride[1] != shape[2] || shape[0] != args.at("query_shape").at(0).get<double>()
            || cache.at("locations").get<double>() != shape[0])
            return std::nullopt;

        result.push_back({
            "ascend.scatter_nd_update_",
            CacheWriteGeometry{ "attention." + std::string(tensor) + "_write",
                               shape[1] * shape[2],
                               stride[0],
                               "ascend.slice.bfloat16.aicore", "ascend.scatter_nd_update.bfloat16.aicore" }
        });
    }
    return result;
}

} // namespace

void ProbeReader::observe(const ProbeEvent & event) {
    const auto & name = event.name;
    const auto & args = event.args;
    const auto & process = event.process;
    if (name == "runtime.hicache.decode_allocation") {
        ComputeFrame frame{ trace_graph::planning::Phase::Decode,      {},          args.at("sequence_lens_tokens").get<std::vector<double>>(), {},
                            args.at("page_size_tokens").get<double>(), event.record };
        for (const auto & identity : args.at("request_ids")) frame.requests.push_back(output_.name(identity.get<std::string>()));
        frame.query_tokens = std::vector<double>(frame.requests.size(), args.at("token_per_req").get<double>());
        forward_frames_[process] = output_.compute_frames.size();
        output_.compute_frames.push_back(std::move(frame));
    }

    if (name == "hicache_cache_lookup_input_end" || name == "hicache_cache_finished_req_lifecycle_commit_end") {
        const auto path = token_path(args.at("token_dictionary"));
        const auto role = name == "hicache_cache_lookup_input_end" ? TokenRole::LookupInput : TokenRole::Committed;
        output_.observe_tokens(args.at("request_id").get<std::string>(), role, path, event.record);
    }
    if (name == "hicache_cache_extend_input_start") {
        const auto & dictionaries = args.at("token_dictionaries");
        const auto & identities = args.at("request_ids");
        ComputeFrame frame{ trace_graph::planning::Phase::Prefill, {}, {}, std::nullopt, std::nullopt, event.record };
        for (std::size_t i = 0; i < identities.size(); ++i) {
            const auto path = token_path(dictionaries.at(i));
            const auto request = identities[i].get<std::string>();
            output_.observe_tokens(request, TokenRole::Prompt, path, event.record);
            frame.requests.push_back(output_.name(request));
            frame.sequence_tokens.push_back(output_.token_paths.length(path));
        }
        forward_frames_[process] = output_.compute_frames.size();
        output_.compute_frames.push_back(std::move(frame));
    }

    if (name == "runtime.phase.linear" || name == "runtime.phase.logits_projection" || name == "runtime.phase.normalization"
        || name == "runtime.phase.gated_activation" || name == "runtime.phase.attention")
        observe_compute(event);
}

trace_graph::planning::PrefixId ProbeReader::token_path(const nlohmann::json & dictionary) {
    const auto identity = dictionary.at("token_path_id").get<std::string>();
    if (!dictionary.contains("token_ids")) return token_descriptors_.at(identity);

    // Native probes declare each payload once, then refer to its dictionary ID.
    // Resolve that protocol here; target identity uses the actual token ancestry.
    const auto tokens = dictionary.at("token_ids").get<std::vector<std::int64_t>>();
    const auto path = output_.token_paths.intern(tokens);
    const auto [entry, inserted] = token_descriptors_.try_emplace(identity, path);
    if (!inserted && entry->second != path) throw std::runtime_error("Source token dictionary reuses an ID for different content");
    return path;
}

const AttentionDescriptor & ProbeReader::attention_descriptor(const ProbeEvent & event, trace_graph::planning::Phase phase) {
    using namespace trace_graph::planning;
    const auto & args = event.args;
    const auto key = std::tuple(event.process, phase, args.at("descriptor_id").get<Index>());
    if (!args.at("descriptor_present").get<bool>()) return attention_descriptors_.at(key);

    AttentionDescriptor result{
        std::nullopt, {}, args.at("page_size_tokens").get<double>(), args.at("query_shape").at(0).get<double>(), std::nullopt, args.at("layer_id").get<Index>()
    };
    if (!args.at("extend_query_lengths").is_null()) result.query_tokens = args.at("extend_query_lengths").get<std::vector<double>>();
    const auto d = args.at("query_key_head_dim").get<double>(), v = args.at("value_head_dim").get<double>();
    // These are the eager Ascend FIA branches; graph, sliding-window and
    // grouped requests need their own execution and work descriptions.
    const bool supported =
        args.at("use_fia").get<bool>() && !args.at("use_mla").get<bool>() && args.at("use_alibi") != true && !args.at("graph_mode").get<bool>()
        && !args.at("cross_attention").get<bool>() && !args.at("encoder_present").get<bool>() && !args.at("sinks_present").get<bool>()
        && !args.at("mode_is_target_verify").get<bool>() && !args.at("mode_is_draft_extend").get<bool>() && args.at("sliding_window_size").get<int>() == -1
        && args.at("attention_cp_size").get<int>() == 1 && args.at("batch_size").get<int>() == 1 && args.at("value_heads") == args.at("key_heads")
        && (phase == Phase::Prefill ? ((d == v && (d == 128 || d == 192 || d == 256)) || (d == 192 && v == 128))
                                    : args.at("fia_torch_ops_dispatch") == "batch_invariant");
    const auto writes = attention_writes(args);
    if (supported && writes) {
        const auto implementation = phase == Phase::Prefill ? "ascend.fia.TND" : "ascend.fia.batch_invariant.BSND";
        result.operation = ComputeOperation{
            implementation,
            AttentionGeometry{ args.at("query_heads").get<double>(), args.at("key_heads").get<double>(), d, v, args.at("causal_attention").get<bool>() }
        };
        result.cache_writes = *writes;
    }
    const auto [stored, inserted] = attention_descriptors_.try_emplace(key, result);
    if (!inserted && stored->second != result) throw std::runtime_error("Attention descriptor ID is redefined with different work");
    return stored->second;
}

void ProbeReader::observe_compute(const ProbeEvent & event) {
    const auto & args = event.args;
    if (args.at("status") != "returned") return;
    const auto context = forward_frames_.find(event.process);
    if (context == forward_frames_.end()) {
        ++output_.counts["compute_work_without_forward_context"];
        return;
    }
    auto & frame = output_.compute_frames[context->second];
    const AttentionDescriptor * descriptor = nullptr;
    if (event.name == "runtime.phase.attention") {
        const auto phase = frame.phase == trace_graph::planning::Phase::Prefill ? "prefill" : "decode";
        if (args.at("phase") != phase) {
            ++output_.counts["compute_frame_phase_mismatch"];
            return;
        }
        descriptor = &attention_descriptor(event, frame.phase);
        frame.page_tokens = descriptor->page_tokens;
        if (frame.phase == trace_graph::planning::Phase::Prefill) frame.query_tokens = descriptor->query_tokens;
    }

    auto observed = read_compute(event, frame, descriptor);
    if (observed.empty()) {
        ++output_.counts["compute_work_unsupported_geometry." + event.name];
        return;
    }

    // The probe supplies geometry and provenance, not a device measurement.
    // The independent provider estimates service; this CPU envelope is not charged.
    const auto resource = output_.resource(ResourceKind::CpuThread, "cpu:" + event.process + ":" + event.thread);
    std::optional<Index> layer;
    if (descriptor) layer = descriptor->layer;
    else if (args.contains("model_layer_index") && !args.at("model_layer_index").is_null()) layer = args.at("model_layer_index").get<Index>();
    else if (args.contains("layer_id") && !args.at("layer_id").is_null()) layer = args.at("layer_id").get<Index>();
    std::vector<trace_graph::planning::Work> work;
    std::vector<trace_graph::planning::ComputePart> recipe;
    for (auto & part : observed) {
        for (auto & operation : part.operation.describe(frame.phase, part.input)) work.push_back(std::move(operation));
        recipe.push_back({ std::move(part.operation), part.rows });
    }
    const auto description = output_.describe_work(std::move(work), std::move(recipe));
    output_.work_observations.push_back({ description, resource, event.begin_ns, event.end_ns, event.record, context->second, layer });
}

} // namespace markov::adapters::sglang
