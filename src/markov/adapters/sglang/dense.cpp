#include "markov/adapters/sglang/dense.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <utility>

namespace markov::adapters::sglang {

using namespace trace_graph::planning;

namespace {

ComputeObservation tensor(std::string operation, std::string implementation, double query, double batch, Features fixed, Features per_query,
                          Features per_request = Features{}) {
    return {
        { std::move(implementation), TensorGeometry{ std::move(operation), std::move(fixed), std::move(per_query), std::move(per_request) } },
        AttentionInput{ query, query, query, batch, 0 },
        std::nullopt
    };
}

std::vector<ComputeObservation> read_tensor_work(const ProbeEvent & event) {
    const auto & a = event.args;
    const auto & name = event.name;
    if (name == "runtime.phase.embedding_forward") {
        if (a.at("implementation") != "sglang.VocabParallelEmbedding.forward"
            || a.at("quant_implementation") != "sglang.srt.layers.quantization.unquant.UnquantizedEmbeddingMethod" || a.at("input_shape").size() != 1
            || a.at("output_shape").size() != 2)
            return {};

        const double width = a.at("output_shape").at(1);
        std::vector<ComputeObservation> result;
        result.push_back(tensor("embedding_output_mask",
                                "aten.masked_fill_:embedding:" + a.at("output_dtype").get<std::string>(),
                                a.at("input_shape").at(0),
                                1,
                                Features{
                                    { "width", width }
        },
                                Features{ { "rows", 1 }, { "elements", width } }));
        return result;
    }
    if (name == "runtime.phase.embedding") {
        if (a.at("implementation") != "torch.nn.functional.embedding" || a.at("indices_dtype") != "torch.int64" || a.at("indices_shape").size() != 1
            || a.at("weight_shape").size() != 2 || a.at("indices_stride") != nlohmann::json::array({ 1 }))
            return {};

        const double width = a.at("weight_shape").at(1);
        std::vector<ComputeObservation> result;
        result.push_back(tensor("embedding_lookup",
                                "torch.embedding:" + a.at("weight_dtype").get<std::string>() + ":index=torch.int64",
                                a.at("indices_shape").at(0),
                                1,
                                Features{
                                    {           "width",                                    width },
                                    { "vocabulary_rows", a.at("weight_shape").at(0).get<double>() }
        },
                                Features{ { "rows", 1 }, { "output_elements", width } }));
        return result;
    }
    if (name == "runtime.phase.embedding_mask") {
        if (a.at("implementation") != "sglang.get_masked_input_and_mask.npu_eager" || a.at("input_dtype") != "torch.int64" || a.at("input_shape").size() != 1)
            return {};

        const double query = a.at("input_shape").at(0);
        std::vector<ComputeObservation> result;

        // Eager NPU executes the comparisons and implicit bool promotions;
        // these are tensor operations, not sixteen fitted CPU envelope charges.
        for (const auto & [operation, implementation] : std::array<std::pair<const char *, const char *>, 16>{
                 { { "org_lower", "aten.ge:sglang.embedding_mask:int64" },
                  { "org_upper", "aten.lt:sglang.embedding_mask:int64" },
                  { "org_mask", "aten.bitwise_and:sglang.embedding_mask:int64" },
                  { "added_lower", "aten.ge:sglang.embedding_mask:int64" },
                  { "added_upper", "aten.lt:sglang.embedding_mask:int64" },
                  { "added_mask", "aten.bitwise_and:sglang.embedding_mask:int64" },
                  { "org_offset.promote", "ascend.cast.bool_to_int64" },
                  { "org_offset.multiply", "ascend.mul.int64.aicore" },
                  { "added_offset.promote", "ascend.cast.bool_to_int64" },
                  { "added_offset.multiply", "ascend.mul.int64.aicore" },
                  { "valid_offset", "aten.add:sglang.embedding_mask:int64" },
                  { "vocab_mask", "aten.bitwise_or:sglang.embedding_mask:int64" },
                  { "subtract_offset", "aten.sub:sglang.embedding_mask:int64" },
                  { "remap_indices.promote", "ascend.cast.bool_to_int64" },
                  { "remap_indices.multiply", "ascend.mul.int64.aicore" },
                  { "invalid_mask", "aten.bitwise_not:sglang.embedding_mask:int64" } }
        })
            result.push_back(tensor("embedding_mask." + std::string(operation),
                                    implementation,
                                    query,
                                    1,
                                    Features{
            },
                                    Features{ { "rows", 1 }, { "elements", 1 } }));
        return result;
    }
    if (name == "runtime.phase.input_buffers") {
        if (a.at("implementation") != "sglang.EagerRunner.load_batch" || !a.at("copy_enabled").get<bool>() || a.at("pipeline_inputs_present").get<bool>())
            return {};

        struct Copies {
            std::string source_type, destination_type;
            double tensors = 0, per_query = 0, per_batch = 0;
        };
        std::vector<Copies> groups;
        for (const auto & slot : a.at("slots")) {
            if (slot.at("custom_source").get<bool>() || slot.at("post_fill").get<bool>()) return {};
            if (!slot.at("copy_from_batch").get<bool>() || !slot.at("source_tensor").get<bool>()) continue;
            if (slot.at("custom_slice").get<bool>() || slot.at("source_device") != slot.at("buffer_device")) return {};
            if (slot.at("buffer_device") == "cpu") {
                // The measured host preparation owns same-dtype batch lengths.
                if (slot.at("axis") != "bs" || slot.at("source_dtype") != slot.at("buffer_dtype") || slot.at("source_shape").size() != 1) return {};
                continue;
            }
            if (slot.at("buffer_device") != "npu") return {};
            double width = 1;
            for (std::size_t i = 1; i < slot.at("source_shape").size(); ++i) width *= slot.at("source_shape").at(i).get<double>();
            const auto source_type = slot.at("source_dtype").get<std::string>(), destination_type = slot.at("buffer_dtype").get<std::string>();
            auto group =
                std::ranges::find_if(groups, [&](const Copies & copy) { return copy.source_type == source_type && copy.destination_type == destination_type; });
            if (group == groups.end()) group = groups.insert(groups.end(), { source_type, destination_type });
            ++group->tensors;

            // Mixed-dtype foreach submits separately for each converted tensor.
            if (source_type != destination_type && group->tensors > 1) return {};
            if (slot.at("axis") == "tokens") group->per_query += width;
            else if (slot.at("axis") == "bs") group->per_batch += width;
            else return {};
        }
        std::vector<ComputeObservation> result;
        for (const auto & group : groups)
            result.push_back(tensor("input_buffers.copy",
                                    "torch._foreach_copy_:" + group.source_type + ":" + group.destination_type,
                                    a.at("input_tokens"),
                                    a.at("batch_size"),
                                    Features{
                                        { "tensor_count", group.tensors }
            },
                                    Features{ { "elements", group.per_query } },
                                    Features{ { "elements", group.per_batch } }));
        return result;
    }
    if (name == "runtime.phase.rotary") {
        if (a.at("implementation") != "sglang.RotaryEmbedding.forward_npu" || a.at("offsets_present").get<bool>() || a.at("fused_kv_write").get<bool>()
            || a.at("cached_position_table").get<bool>() || !a.at("native_rotary_uncompiled").get<bool>() || a.at("query_input_dtype") != "torch.bfloat16"
            || a.at("table_dtype") != "torch.bfloat16" || a.at("positions_dtype") != "torch.int64" || a.at("query_input_shape").size() != 2
            || a.at("key_input_shape").size() != 2 || a.at("rotary_dim") != a.at("head_dim"))
            return {};
        std::vector<ComputeObservation> result;
        result.push_back(tensor("rotary",
                                "torch_npu.mrope:torch.bfloat16:table=torch.bfloat16:index=torch.int64",
                                a.at("query_input_shape").at(0),
                                1,
                                Features{
                                    { "query_width", a.at("query_input_shape").at(1).get<double>() },
                                    {    "kv_width",   a.at("key_input_shape").at(1).get<double>() },
                                    {    "head_dim",                a.at("head_dim").get<double>() },
                                    {  "table_rows",       a.at("table_shape").at(0).get<double>() },
                                    {  "neox_style",     a.at("is_neox_style").get<bool>() ? 1 : 0 }
        },
                                Features{ { "rows", 1 } }));
        return result;
    }
    if (name == "runtime.phase.rotary_table") {
        if (a.at("implementation") != "sglang.RotaryEmbedding.get_cos_sin_with_position" || a.at("positions_shape").size() != 1
            || a.at("positions_dtype") != "torch.int64" || a.at("table_shape").size() != 2)
            return {};

        const double width = a.at("table_shape").at(1);
        const Features fixed{
            {      "width",                                   width },
            { "table_rows", a.at("table_shape").at(0).get<double>() }
        };
        const Features scaled{
            {     "rows",     1 },
            { "elements", width }
        };
        const auto types = ":sglang.rotary_table:" + a.at("table_dtype").get<std::string>() + ":index=torch.int64";
        std::vector<ComputeObservation> result;
        result.push_back(tensor("rotary_table.gather", "aten.index_select" + types, a.at("positions_shape").at(0), 1, fixed, scaled));
        result.push_back(tensor("rotary_table.repeat", "aten.repeat" + types, a.at("positions_shape").at(0), 1, fixed, scaled));
        for (const auto * output : { "cos_copy", "sin_copy" }) {
            auto copy = tensor(std::string("rotary_table.") + output, "aten.clone" + types, a.at("positions_shape").at(0), 1, fixed, scaled);

            // Chunked outputs have row stride 2*width; a single row is a view.
            std::get<TensorGeometry>(copy.operation.geometry).single_query_view = true;
            result.push_back(std::move(copy));
        }
        return result;
    }
    if (name == "runtime.phase.qkv_norm_rope") {
        if (a.at("implementation") != "sgl_kernel_npu.split_qkv_rmsnorm_rope" || !a.at("query_bias_shape").is_null() || !a.at("key_bias_shape").is_null()
            || a.at("input_shape").size() != 2 || a.at("query_weight_dtype") != a.at("key_weight_dtype") || a.at("sin_dtype") != a.at("cos_dtype"))
            return {};

        const auto implementation = "sgl_kernel_npu.split_qkv_rmsnorm_rope:" + a.at("input_dtype").get<std::string>()
                                    + ":weight=" + a.at("query_weight_dtype").get<std::string>() + ":rope=" + a.at("sin_dtype").get<std::string>();
        std::vector<ComputeObservation> result;
        result.push_back(tensor("qkv_norm_rope",
                                implementation,
                                a.at("input_shape").at(0),
                                1,
                                Features{
                                    {               "query_width",                     a.at("query_width").get<double>() },
                                    {                  "kv_width",                        a.at("kv_width").get<double>() },
                                    {                  "head_dim",                        a.at("head_dim").get<double>() },
                                    {                  "rope_dim",                a.at("sin_shape").back().get<double>() },
                                    {                   "epsilon",                         a.at("epsilon").get<double>() },
                                    {                "neox_style",             a.at("is_neox_style").get<bool>() ? 1 : 0 },
                                    { "triton_task_queue_enabled", a.at("triton_task_queue_enabled").get<bool>() ? 1 : 0 }
        },
                                Features{ { "rows", 1 } }));
        return result;
    }
    if (name == "runtime.phase.logits_output") {
        if (a.at("implementation") != "sglang.LogitsProcessor._copy_logits_to_buffer" || a.at("output_buffer_present").get<bool>()
            || !a.at("last_token_projection").get<bool>() || a.at("input_shape").size() != 2 || a.at("output_shape").size() != 2)
            return {};

        const double vocab = a.at("vocab_size");
        std::vector<ComputeObservation> result;
        result.push_back(tensor("logits_output",
                                "aten._to_copy:logits:" + a.at("input_dtype").get<std::string>() + ":" + a.at("output_dtype").get<std::string>(),
                                0,
                                a.at("input_shape").at(0),
                                Features{
                                    {      "input_width", a.at("input_shape").at(1).get<double>() },
                                    { "vocabulary_width",                                   vocab }
        },
                                Features{},
                                Features{ { "rows", 1 }, { "output_elements", vocab } }));
        return result;
    }
    if (name == "runtime.phase.greedy_sampling") {
        if (a.at("implementation") != "sglang.Sampler.greedy_argmax" || a.at("input_dtype") != "torch.float32" || a.at("output_dtype") != "torch.int64"
            || a.at("input_shape").size() != 2)
            return {};

        const double width = a.at("input_shape").at(1), batch = a.at("input_shape").at(0);
        std::vector<ComputeObservation> result;
        result.push_back(tensor("greedy_argmax.reduce",
                                "ascend.argmax.float32_to_int32.aicore",
                                0,
                                batch,
                                Features{
                                    { "vocabulary_width", width }
        },
                                Features{},
                                Features{ { "rows", 1 }, { "elements", width } }));
        result.push_back(tensor("greedy_argmax.cast",
                                "ascend.cast.int32_to_int64",
                                0,
                                batch,
                                Features{
        },
                                Features{},
                                Features{ { "elements", 1 } }));
        return result;
    }
    if (name == "runtime.phase.logits_pruning") {
        if (a.at("passthrough").get<bool>()) return std::vector<ComputeObservation>{};
        if (a.at("implementation") != "sglang.LogitsProcessor._get_pruned_states" || !a.at("last_token_selection").get<bool>()
            || a.at("before_norm_present").get<bool>() || a.at("auxiliary_count") != 0 || a.at("padded_static_len") != -1
            || a.at("length_dtype") != "torch.int32" || a.at("length_device_type") != "npu")
            return {};

        const double query = a.at("input_shape").at(0), batch = a.at("length_shape").at(0);
        std::vector<ComputeObservation> result;
        result.push_back(tensor("logits_pruning.length_cast",
                                "ascend.cast.int32_to_int64",
                                query,
                                batch,
                                Features{
        },
                                Features{},
                                Features{ { "elements", 1 } }));
        result.push_back(tensor("logits_pruning.length_sum",
                                "ascend.cumsum.int64.aicpu",
                                query,
                                batch,
                                Features{
        },
                                Features{},
                                Features{ { "elements", 1 } }));
        result.push_back(tensor("logits_pruning.last_index",
                                "aten.sub:torch.int64:scalar",
                                query,
                                batch,
                                Features{
        },
                                Features{},
                                Features{ { "elements", 1 } }));
        result.push_back(tensor("logits_pruning.select",
                                "aten.index:logits_hidden:" + a.at("input_dtype").get<std::string>(),
                                query,
                                batch,
                                Features{
                                    { "width", a.at("input_shape").at(1).get<double>() }
        },
                                Features{ { "input_rows", 1 } },
                                Features{ { "selected_rows", 1 } }));
        return result;
    }
    return {};
}

} // namespace

std::vector<ComputeObservation> read_dense_work(const ProbeEvent & event) {
    const auto & args = event.args;
    auto tensor_work = read_tensor_work(event);
    if (!tensor_work.empty()) return tensor_work;
    if (!args.contains("input_shape") || !args.contains("input_stride")) return {};
    const auto input = args.at("input_shape").get<std::vector<double>>();
    const auto stride = args.at("input_stride").get<std::vector<double>>();
    if (input.size() != 2 || stride.size() != 2) return {};
    const std::array<double, 2> input_stride{ stride[0], stride[1] };
    const auto basis =
        event.name == "runtime.phase.logits_projection" && args.at("last_token_projection").get<bool>() ? RowBasis::BatchSize : RowBasis::QueryTokens;
    std::optional<RowBinding> rows;
    if (args.contains("rows_per_token") && !args.at("rows_per_token").is_null()) rows = RowBinding{ basis, args.at("rows_per_token").get<double>() };

    if (event.name == "runtime.phase.qk_reshape" && args.at("implementation") == "sglang.reshape_for_qk_norm" && stride[1] == 1) {
        const ReshapeGeometry shape{ input[1], args.at("head_dim").get<double>(), stride[0] };
        if (shape.is_view(input[0]) != args.at("storage_shared").get<bool>()) return {};

        return {
            ComputeObservation{ { "aten.clone:qk_reshape:" + args.at("input_dtype").get<std::string>(), shape }, RowInput{ input[0] }, rows }
        };
    }

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

} // namespace markov::adapters::sglang
