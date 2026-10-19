#include "markov/adapters/frameworks/sglang/models/model_kernel_layout.hpp"
#include "markov/production/block_binding.hpp"

#include "markov/contracts/source_observations.hpp"
#include "markov/graph/kernel_shapes.hpp"
#include "markov/graph/source_graph.hpp"

#include <algorithm>
#include <string_view>
#include <vector>

namespace markov {

ModelKernelLayout::ModelKernelLayout(ModelKernelRole role, std::int64_t output_width) : role_(role), output_width_(output_width) {}

std::optional<ModelKernelLayout> ModelKernelLayout::bind_page_table(const SourceKernel & kernel, std::span<const SourceRequest> requests,
                                                                    std::uint32_t page_size_tokens) {
    const bool maximum = kernel.name == "aclnnMax_ReduceMaxAiCore_ReduceMax";
    if (!maximum && !kernel.name.starts_with("aclnnFloorDivides_")) return std::nullopt;
    std::int64_t max_sequence = 0;
    for (const auto & request : requests) { max_sequence = std::max(max_sequence, static_cast<std::int64_t>(request.context_tokens) + request.input_tokens); }
    const auto batch = static_cast<std::int64_t>(requests.size());
    const auto columns = (max_sequence + page_size_tokens - 1) / page_size_tokens;
    const KernelShapes inputs(kernel.input_shapes);
    const KernelShapes outputs(kernel.output_shapes);
    const std::vector<std::int64_t> matrix{ batch, columns };
    ModelKernelLayout layout(ModelKernelRole::PageTableCast, maximum ? 1 : columns);
    layout.source_requests_ = batch;
    layout.page_size_tokens_ = page_size_tokens;
    layout.input_token_axes_ = {
        { 0, 1,    AxisExtent::Requests, 0 },
        { 0, 1, AxisExtent::PageColumns, 1 }
    };
    layout.output_token_axes_ = layout.input_token_axes_;
    std::vector<std::vector<std::int64_t>> expected;
    std::vector<std::vector<std::int64_t>> expected_outputs{ matrix };

    // Ascend's non-graph metadata gathers request rows, selects each page's
    // first token location, then divides by page_size. Descriptors are constants.
    if (maximum && kernel.operation == "ReduceMax" && kernel.input_dtypes == "INT64;INT64" && kernel.output_dtypes == "INT64") {
        layout.role_ = ModelKernelRole::SequenceLengthMaximum;
        expected = { { batch }, { 1 } };
        expected_outputs = { {} };
        layout.input_token_axes_ = {
            { 0, 1, AxisExtent::Requests }
        };
        layout.output_token_axes_.clear();
    }
    else if (kernel.operation == "Cast"
             && ((kernel.input_dtypes == "INT32" && kernel.output_dtypes == "INT64") || (kernel.input_dtypes == "INT64" && kernel.output_dtypes == "INT32"))) {
        expected = { matrix };
    }
    else if (kernel.operation == "FloorDiv" && kernel.input_dtypes == "INT64;INT64" && kernel.output_dtypes == "INT64") {
        layout.role_ = ModelKernelRole::PageTableDivide;
        expected = { matrix, {} };
    }
    else if (kernel.operation == "Slice" && kernel.input_dtypes == "INT32;INT64;INT64" && kernel.output_dtypes == "INT32" && columns == 1 && batch > 1
             && max_sequence > 1) {
        layout.role_ = ModelKernelRole::PageTableSlice;
        expected = {
            { batch, max_sequence },
            { 2 },
            { 2 }
        };
        layout.input_token_axes_[1].extent = AxisExtent::SequenceTokens;
    }
    else if (kernel.operation == "AsStrided" && kernel.input_dtypes == "INT32;INT64;INT64;INT64" && kernel.output_dtypes == "INT32" && columns > 1
             && page_size_tokens > 1) {
        layout.role_ = ModelKernelRole::PageTableView;
        expected = { { batch * max_sequence }, { 2 }, { 2 }, { 1 } };
        layout.input_token_axes_ = {
            { 0, 1, AxisExtent::SequenceStorage }
        };
    }
    else return std::nullopt;

    if (inputs.tensors != expected || outputs.tensors != expected_outputs) return std::nullopt;
    return layout;
}

std::optional<ModelKernelLayout> ModelKernelLayout::bind_forward_inputs(const SourceKernel & kernel, std::int64_t tokens, std::int64_t requests, StepKind step,
                                                                        bool foreach_copy) {
    const KernelShapes inputs(kernel.input_shapes);
    const KernelShapes outputs(kernel.output_shapes);
    ModelKernelLayout layout(ModelKernelRole::ForwardPositions, 1);
    layout.source_requests_ = requests;
    layout.input_token_axes_ = {
        { 0, 1, AxisExtent::Requests }
    };
    layout.output_token_axes_ = {
        { 0, 1, AxisExtent::Requests }
    };

    if (foreach_copy && kernel.name == "ForeachCopy" && kernel.operation == "ForeachCopy") {
        // Eager registry groups input_ids/positions (T) and request slots/lengths (B).
        // NPUPagedTokenToKVPoolAllocator returns INT32 cache locations, which
        // belong to the separate INT32-to-INT64 copy group.
        const std::vector<std::vector<std::int64_t>> expected{ { tokens }, { tokens }, { requests }, { requests } };
        if (kernel.input_dtypes != "INT64;INT64;INT64;INT64" || kernel.output_dtypes != kernel.input_dtypes || inputs.tensors != expected
            || outputs.tensors != expected)
            return std::nullopt;

        layout.role_ = ModelKernelRole::ShapeOnly;
        layout.input_token_axes_ = {
            { 0, 1,   AxisExtent::Tokens },
            { 1, 1,   AxisExtent::Tokens },
            { 2, 1, AxisExtent::Requests },
            { 3, 1, AxisExtent::Requests }
        };
        layout.output_token_axes_ = layout.input_token_axes_;
        return layout;
    }

    if (foreach_copy && kernel.name == "aclnnInplaceCopy_CastAiCore_Cast" && kernel.operation == "Cast") {
        // The eager registry copies INT32 allocator slots into its INT64 buffer.
        // Operation identity distinguishes this copy from other forward casts.
        const std::vector<std::vector<std::int64_t>> expected{ { tokens } };
        if (kernel.input_dtypes != "INT32" || kernel.output_dtypes != "INT64" || inputs.tensors != expected || outputs.tensors != expected) return std::nullopt;

        layout.role_ = ModelKernelRole::ShapeOnly;
        layout.input_token_axes_ = {
            { 0, 1, AxisExtent::Tokens }
        };
        layout.output_token_axes_ = layout.input_token_axes_;
        return layout;
    }

    if (step == StepKind::Prefill) {
        if (kernel.name != "compute_position_kernel" || kernel.operation != "compute_position_kernel") return std::nullopt;
        const std::vector<std::vector<std::int64_t>> expected_inputs{ { requests }, { requests } };
        const std::vector<std::vector<std::int64_t>> expected_outputs{ { tokens }, { requests } };
        if (inputs.tensors != expected_inputs || outputs.tensors != expected_outputs || kernel.input_dtypes != "INT32;INT32"
            || kernel.output_dtypes != "INT64;INT32")
            return std::nullopt;

        // Prefix and extend lengths produce concatenated positions and one
        // exclusive extend offset per request.
        layout.input_token_axes_.push_back({ 1, 1, AxisExtent::Requests });
        layout.output_token_axes_ = {
            { 0, 1,   AxisExtent::Tokens },
            { 1, 1, AxisExtent::Requests }
        };
    }
    else if (step == StepKind::Decode && tokens == requests) {
        // Native non-speculative decode uses clamp(seq_lens - 1, min=0).
        // Scalar slots stay fixed; only the per-request vector varies.
        std::vector<std::vector<std::int64_t>> expected_inputs{ { requests }, {} };
        if (kernel.name == "aclnnSubs_SubAiCore_Sub" && kernel.operation == "Sub" && kernel.input_dtypes == "INT64;INT64")
            layout.role_ = ModelKernelRole::ShapeOnly;
        else if (kernel.name == "aclnnClamp_ClipByValueV2AiCore_ClipByValueV2" && kernel.operation == "ClipByValueV2"
                 && kernel.input_dtypes == "INT64;INT64;INT64") {
            layout.role_ = ModelKernelRole::DecodePositionClamp;
            expected_inputs.push_back({});
        }
        else return std::nullopt;

        const std::vector<std::vector<std::int64_t>> expected_outputs{ { requests } };
        if (inputs.tensors != expected_inputs || outputs.tensors != expected_outputs || kernel.output_dtypes != "INT64") return std::nullopt;
    }
    else return std::nullopt;
    return layout;
}

std::optional<ModelKernelLayout> ModelKernelLayout::bind_sequence_lengths(const SourceNativeRegion & region, const SourceKernel & kernel,
                                                                          std::span<const SourceRequest> requests) {
    if (region.semantic != "attention_metadata" || region.method != "init_forward_metadata" || region.parameters.at("is_hybrid_swa") != 0) return std::nullopt;
    if (kernel.name != "aclnnInplaceCopy_CastAiCore_Cast" || kernel.operation != "Cast" || kernel.input_dtypes != "INT64" || kernel.output_dtypes != "INT32")
        return std::nullopt;

    const auto rows = static_cast<std::int64_t>(requests.size());
    const auto & lengths = region.inputs.at("forward_batch.seq_lens");
    const auto & values = region.cpu_values.at("forward_batch.seq_lens_cpu");
    if (!lengths || lengths->dtype != "torch.int64" || lengths->shape != std::vector<std::int64_t>{ rows } || values.size() != requests.size())
        return std::nullopt;

    for (std::size_t index = 0; index < requests.size(); ++index) {
        if (values[index] != static_cast<std::int64_t>(requests[index].context_tokens) + requests[index].input_tokens) return std::nullopt;
    }
    const std::vector<std::vector<std::int64_t>> expected{ { rows } };
    if (KernelShapes(kernel.input_shapes).tensors != expected || KernelShapes(kernel.output_shapes).tensors != expected) return std::nullopt;

    // This is the backend's per-request length vector, distinct from the
    // two-dimensional block table casts in the same metadata scope.
    ModelKernelLayout layout(ModelKernelRole::SequenceLengthsCast, 1);
    layout.source_requests_ = rows;
    layout.input_token_axes_ = {
        { 0, 1, AxisExtent::Requests }
    };
    layout.output_token_axes_ = layout.input_token_axes_;
    return layout;
}

std::optional<ModelKernelLayout> ModelKernelLayout::bind_sampling_positions(const SourceNativeRegion & region, const SourceKernel & kernel,
                                                                            std::span<const SourceRequest> requests, StepKind step) {
    if (step != StepKind::Prefill || region.semantic != "sampling" || region.method != "sample" || region.call_parameters.at("is_all_greedy") != 1
        || region.call_parameters.at("return_logprob") != 0)
        return std::nullopt;
    if (kernel.name != "aclnnSubs_SubAiCore_Sub" || kernel.operation != "Sub" || kernel.input_dtypes != "INT64;INT64" || kernel.output_dtypes != "INT64")
        return std::nullopt;

    const auto rows = static_cast<std::int64_t>(requests.size());
    const auto & lengths = region.inputs.at("forward_batch.seq_lens");
    if (!lengths || lengths->dtype != "torch.int64" || lengths->shape != std::vector<std::int64_t>{ rows }) return std::nullopt;
    const auto & values = region.cpu_values.at("forward_batch.seq_lens_cpu");
    if (values.size() != requests.size()) return std::nullopt;
    for (std::size_t index = 0; index < requests.size(); ++index) {
        if (values[index] != static_cast<std::int64_t>(requests[index].context_tokens) + requests[index].input_tokens) return std::nullopt;
    }

    const KernelShapes inputs(kernel.input_shapes);
    const KernelShapes outputs(kernel.output_shapes);
    if (inputs.tensors != std::vector<std::vector<std::int64_t>>{ { rows }, {} } || outputs.tensors != std::vector<std::vector<std::int64_t>>{ { rows } })
        return std::nullopt;

    ModelKernelLayout layout(ModelKernelRole::SamplingPositions, 1);
    layout.source_requests_ = rows;
    layout.input_token_axes_ = {
        { 0, 1, AxisExtent::Requests }
    };
    layout.output_token_axes_ = layout.input_token_axes_;
    return layout;
}

std::optional<ModelKernelLayout> ModelKernelLayout::bind_sampling(const SourceKernel & kernel) const {
    if (role_ != ModelKernelRole::LogitsCast) return std::nullopt;

    // Native greedy sampling reduces the vocabulary axis of the qualified
    // logits output. B rows are independent of prefill's total token count.
    const KernelShapes inputs(kernel.input_shapes);
    const KernelShapes outputs(kernel.output_shapes);
    ModelKernelLayout layout(ModelKernelRole::ShapeOnly, 1);
    std::vector<std::vector<std::int64_t>> expected;
    if (kernel.name == "aclnnArgMax_ArgMaxV2AiCore_ArgMaxV2" && kernel.operation == "ArgMaxV2" && kernel.input_dtypes == "FLOAT;INT64"
        && kernel.output_dtypes == "INT32") {
        expected = {
            { source_requests_, output_width_ },
            {}
        };
    }
    else if (kernel.name == "aclnnArgMax_CastAiCore_Cast" && kernel.operation == "Cast" && kernel.input_dtypes == "INT32" && kernel.output_dtypes == "INT64") {
        expected = { { source_requests_ } };
    }
    else return std::nullopt;

    if (inputs.tensors != expected || outputs.tensors != std::vector<std::vector<std::int64_t>>{ { source_requests_ } }) return std::nullopt;
    layout.source_requests_ = source_requests_;
    layout.input_token_axes_ = {
        { 0, 1, AxisExtent::Requests }
    };
    layout.output_token_axes_ = layout.input_token_axes_;
    return layout;
}

bool ModelKernelLayout::matches(const KernelShapes & inputs, const KernelShapes & outputs, std::int64_t tokens, std::int64_t input_width,
                                std::int64_t output_width, bool linear) {
    std::vector<std::vector<std::int64_t>> expected{
        { tokens, input_width }
    };
    if (linear) expected.push_back({ output_width, input_width });
    const std::vector<std::vector<std::int64_t>> expected_outputs{
        { tokens, output_width }
    };
    return inputs.tensors == expected && outputs.tensors == expected_outputs;
}

std::optional<ModelKernelLayout> ModelKernelLayout::bind(const SourceNativeRegion & region, const SourceKernel & kernel, std::int64_t serving_tokens,
                                                         std::int64_t requests, StepKind step, const SourceNativeRegion * parameter_child) {
    if (region.semantic == "logits") return bind_logits_output(region, parameter_child, kernel, serving_tokens, requests, step);
    if (region.semantic == "logits_selection") return bind_logits_selection(region, kernel, serving_tokens, requests, step);
    if (region.semantic == "qkv_preparation" && region.method == "forward_prepare_npu"
        && (kernel.operation == "GatherV3" || kernel.operation == "Tile" || kernel.operation == "Slice"))
        return bind_positions(region, kernel, serving_tokens);
    if (region.semantic == "embedding") return bind_embedding(region, kernel, serving_tokens);
    if (region.semantic == "attention_core") return bind_attention_rows(region, kernel, serving_tokens);
    if (kernel.operation == "RmsNorm" || kernel.operation == "AddRmsNorm") return bind_norm(region, kernel, serving_tokens, parameter_child);

    // These rules follow Qwen3's dense projections and Qwen2MLP's merged
    // gate/up -> SwiGLU -> down path. Scope identity selects the native role;
    // equal numeric dimensions in an unrelated operation are not evidence.
    const bool mlp = region.semantic == "feed_forward";
    const bool qkv = region.semantic == "qkv_preparation";
    const bool attention = region.semantic == "attention";
    const bool fused_qkv = qkv && region.method == "forward_prepare_npu" && kernel.operation == "split_qkv_rmsnorm_rope_kernel";
    const bool qkv_slice = qkv && region.method == "forward_prepare_native" && kernel.operation == "Slice";
    const bool rope = qkv && region.method == "forward_prepare_native" && kernel.operation == "RopeWithSinCosCache";
    if (!mlp && !qkv && !attention) return std::nullopt;
    if (kernel.operation != "MatMulV2" && !(mlp && kernel.operation == "SwiGlu") && !fused_qkv && !qkv_slice && !rope) return std::nullopt;

    const auto & activation = region.inputs.at(mlp ? "x" : "hidden_states");
    const auto hidden = region.parameters.at("hidden_size");
    if (!activation || activation->shape != std::vector<std::int64_t>{ serving_tokens, hidden }) return std::nullopt;

    const KernelShapes inputs(kernel.input_shapes);
    const KernelShapes outputs(kernel.output_shapes);

    // Fixed model/TP dimensions come from actual module attributes. The factor
    // two represents the merged gate and up projections, not a fitted factor.
    if (mlp) {
        const auto intermediate = region.parameters.at("intermediate_partition");
        if (kernel.operation == "SwiGlu" && matches(inputs, outputs, serving_tokens, 2 * intermediate, intermediate, false))
            return ModelKernelLayout(ModelKernelRole::ShapeOnly, intermediate);
        if (kernel.operation == "MatMulV2" && matches(inputs, outputs, serving_tokens, hidden, 2 * intermediate, true))
            return ModelKernelLayout(ModelKernelRole::ShapeOnly, 2 * intermediate);
        if (kernel.operation == "MatMulV2" && matches(inputs, outputs, serving_tokens, intermediate, hidden, true))
            return ModelKernelLayout(ModelKernelRole::ShapeOnly, hidden);
        return std::nullopt;
    }

    const auto query = region.parameters.at("query_heads") * region.parameters.at("head_dim");
    const auto kv = region.parameters.at("kv_heads") * region.parameters.at("head_dim");
    if (rope) {
        // The observed device record flattens Q/K inputs, while native mrope
        // returns [T, heads * D]. Cache capacity stays a source property.
        if (inputs.tensors.size() != 4 || inputs.tensors[3].size() != 2 || inputs.tensors[3][1] != region.parameters.at("head_dim")) return std::nullopt;
        const std::vector<std::vector<std::int64_t>> expected_inputs{ { serving_tokens },
                                                                      { serving_tokens * query },
                                                                      { serving_tokens * kv },
                                                                      inputs.tensors[3] };
        const std::vector<std::vector<std::int64_t>> expected_outputs{
            { serving_tokens, query },
            { serving_tokens,    kv }
        };
        if (inputs.tensors != expected_inputs || outputs.tensors != expected_outputs) return std::nullopt;

        ModelKernelLayout layout(ModelKernelRole::ShapeOnly, kv);
        layout.input_token_axes_ = {
            { 0,     1 },
            { 1, query },
            { 2,    kv }
        };
        layout.output_token_axes_ = {
            { 0, 1 },
            { 1, 1 }
        };
        return layout;
    }

    if (qkv_slice) {
        // Native preparation splits the last dimension; materializing a view
        // keeps all token rows. Offset/size operand shapes stay [2]. Their
        // values are not recorded, so this relation does not assign K or V.
        const std::vector<std::vector<std::int64_t>> expected_inputs{
            { serving_tokens, query + 2 * kv },
            { 2 },
            { 2 }
        };
        if (inputs.tensors != expected_inputs || outputs.tensors.size() != 1) return std::nullopt;
        const auto & output = outputs.tensors.front();
        if (output != std::vector<std::int64_t>{ serving_tokens, query } && output != std::vector<std::int64_t>{ serving_tokens, kv }) return std::nullopt;

        return ModelKernelLayout(ModelKernelRole::ShapeOnly, output[1]);
    }

    if (fused_qkv) {
        const auto head_dim = region.parameters.at("head_dim");
        // get_cos_sin_with_position produces [T, 1, 1, D]. At T=1 the
        // observation alone cannot distinguish which singleton axis varies.
        const std::vector<std::vector<std::int64_t>> expected_inputs{
            { serving_tokens, query + 2 * kv },
            { serving_tokens, 1, 1, head_dim },
            { serving_tokens, 1, 1, head_dim }
        };
        const std::vector<std::vector<std::int64_t>> reported_outputs{
            { serving_tokens, query },
            { serving_tokens,    kv }
        };
        if (inputs.tensors != expected_inputs || outputs.tensors != reported_outputs) return std::nullopt;

        // Preserve the partial vendor record. The native function returns
        // three outputs, but this trace cannot identify both K and V operands.
        ModelKernelLayout layout(ModelKernelRole::FusedQkv, query);
        layout.input_token_axes_ = {
            { 0, 1 },
            { 1, 1 },
            { 2, 1 }
        };
        layout.output_token_axes_ = {
            { 0, 1 },
            { 1, 1 }
        };
        return layout;
    }
    if (qkv && matches(inputs, outputs, serving_tokens, hidden, query + 2 * kv, true)) return ModelKernelLayout(ModelKernelRole::ShapeOnly, query + 2 * kv);
    if (attention && matches(inputs, outputs, serving_tokens, query, hidden, true)) return ModelKernelLayout(ModelKernelRole::ShapeOnly, hidden);

    return std::nullopt;
}

std::optional<ModelKernelLayout> ModelKernelLayout::bind_positions(const SourceNativeRegion & region, const SourceKernel & kernel, std::int64_t tokens) {
    const auto & positions = region.inputs.at("positions");
    if (!positions || positions->shape != std::vector<std::int64_t>{ tokens }) return std::nullopt;
    const auto width = region.parameters.at("head_dim");
    const KernelShapes inputs(kernel.input_shapes);
    const KernelShapes outputs(kernel.output_shapes);
    ModelKernelLayout layout(ModelKernelRole::PositionGather, width);
    std::vector<std::vector<std::int64_t>> expected_inputs;
    std::vector<std::vector<std::int64_t>> expected_outputs;

    if (kernel.operation == "GatherV3") {
        if (inputs.tensors.size() != 3 || inputs.tensors[0].size() != 2 || inputs.tensors[0][1] != width) return std::nullopt;
        expected_inputs = { inputs.tensors[0], { tokens }, { 1 } };
        expected_outputs = {
            { tokens, width }
        };
        layout.input_token_axes_ = {
            { 1, 1 }
        };
    }
    else if (kernel.operation == "Tile") {
        // Native reshape(-1, 2, D/2).repeat(1, 1, 2) duplicates each half.
        layout.role_ = ModelKernelRole::PositionTile;
        layout.output_width_ = 2 * width;
        expected_inputs = {
            { tokens, 2, width / 2 },
            { 3 }
        };
        expected_outputs = {
            { tokens, 2, width }
        };
    }
    else {
        // After chunking the two halves, each [T,D] view has row stride 2D.
        // With a single row it is contiguous; multiple rows materialize it.
        if (tokens <= 1) return std::nullopt;
        layout.role_ = ModelKernelRole::PositionSlice;
        expected_inputs = {
            { tokens, 2 * width },
            { 2 },
            { 2 }
        };
        expected_outputs = {
            { tokens, width }
        };
    }
    if (inputs.tensors != expected_inputs || outputs.tensors != expected_outputs) return std::nullopt;
    return layout;
}

std::optional<ModelKernelLayout> ModelKernelLayout::bind_logits_output(const SourceNativeRegion & region, const SourceNativeRegion * selection,
                                                                       const SourceKernel & kernel, std::int64_t tokens, std::int64_t requests, StepKind step) {
    if (!selection) return std::nullopt;
    const auto mode = selection->call_parameters.at("forward_mode");
    // The unique direct selection child establishes the parent invocation's
    // row policy. A same-shaped matrix elsewhere is not evidence for B rows.
    if (!((step == StepKind::Prefill && mode == 1) || (step == StepKind::Decode && mode == 2)) || selection->call_parameters.at("extend_return_logprob") != 0
        || selection->call_parameters.at("padded_static_len") != -1)
        return std::nullopt;
    const auto & hidden = region.inputs.at("hidden_states");
    const auto & selection_input = selection->inputs.at("hidden_states");
    const auto & selected = selection->outputs.at("pruned_states");
    if (!hidden || !selection_input || !selected || hidden->shape.size() != 2 || hidden->shape[0] != tokens || selection_input->shape != hidden->shape
        || selected->shape != std::vector<std::int64_t>{ requests, hidden->shape[1] } || (step == StepKind::Decode && tokens != requests))
        return std::nullopt;

    const KernelShapes inputs(kernel.input_shapes);
    const KernelShapes outputs(kernel.output_shapes);
    const auto hidden_width = hidden->shape[1];
    ModelKernelRole role;
    std::int64_t input_width;
    std::int64_t output_width;
    if (kernel.operation == "MatMulV2") {
        if (inputs.tensors.size() != 2 || inputs.tensors[1].size() != 2) return std::nullopt;
        input_width = hidden_width;
        output_width = inputs.tensors[1][0];
        if (!matches(inputs, outputs, requests, input_width, output_width, true)) return std::nullopt;
        role = ModelKernelRole::LogitsProjection;
    }
    else if (kernel.operation == "Cast") {
        if (inputs.tensors.size() != 1 || inputs.tensors[0].size() != 2 || inputs.tensors[0][0] != requests || outputs.tensors != inputs.tensors
            || kernel.input_dtypes != "DT_BF16" || kernel.output_dtypes != "FLOAT")
            return std::nullopt;
        input_width = output_width = inputs.tensors[0][1];
        role = ModelKernelRole::LogitsCast;
    }
    else if (kernel.operation == "Transpose") {
        // Native TP gather produces [rank,B,V_rank], then permutes (1,0,2).
        // Keep the observed rank and vocabulary axes fixed, even when rank=B.
        if (inputs.tensors.size() != 2 || inputs.tensors[0].size() != 3 || inputs.tensors[1] != std::vector<std::int64_t>{ 3 }) return std::nullopt;
        const auto & shape = inputs.tensors[0];
        if (requests <= 1 || shape[0] <= 1 || shape[1] != requests
            || outputs.tensors
                   != std::vector<std::vector<std::int64_t>>{
                       { requests, shape[0], shape[2] }
        })
            return std::nullopt;
        input_width = output_width = shape[0] * shape[2];
        role = ModelKernelRole::LogitsTranspose;
    }
    else return std::nullopt;

    ModelKernelLayout layout(role, output_width);
    layout.source_requests_ = requests;
    layout.input_token_axes_ = {
        { 0, 1, AxisExtent::Requests }
    };
    layout.output_token_axes_ = {
        { 0, 1, AxisExtent::Requests }
    };
    if (role == ModelKernelRole::LogitsTranspose) layout.input_token_axes_[0].axis = 1;
    return layout;
}

std::optional<ModelKernelLayout> ModelKernelLayout::bind_logits_selection(const SourceNativeRegion & region, const SourceKernel & kernel, std::int64_t tokens,
                                                                          std::int64_t requests, StepKind step) {
    // SGLang ForwardMode::EXTEND is 1 in the bound native implementation.
    // Padding, logprob, decode and speculative paths have separate semantics.
    if (step != StepKind::Prefill || region.call_parameters.at("forward_mode") != 1 || region.call_parameters.at("extend_return_logprob") != 0
        || region.call_parameters.at("padded_static_len") != -1)
        return std::nullopt;
    const auto & hidden = region.inputs.at("hidden_states");
    const auto & selected = region.outputs.at("pruned_states");
    if (!hidden || !selected || hidden->shape.size() != 2 || hidden->shape[0] != tokens
        || selected->shape != std::vector<std::int64_t>{ requests, hidden->shape[1] })
        return std::nullopt;

    const auto width = hidden->shape[1];
    ModelKernelLayout layout(ModelKernelRole::LogitsSelect, 1);
    layout.source_requests_ = requests;
    layout.input_token_axes_ = {
        { 0, 1, AxisExtent::Requests }
    };
    layout.output_token_axes_ = {
        { 0, 1, AxisExtent::Requests }
    };
    std::vector<std::vector<std::int64_t>> expected_inputs{ { requests } };
    std::vector<std::vector<std::int64_t>> expected_outputs{ { requests } };

    if (kernel.operation == "Cast") layout.role_ = ModelKernelRole::ShapeOnly;
    else if (kernel.operation == "Cumsum") {
        layout.role_ = ModelKernelRole::ShapeOnly;
        expected_inputs.push_back({ 1 });
    }
    else if (kernel.operation == "Sub") {
        layout.role_ = ModelKernelRole::ShapeOnly;
        expected_inputs.push_back({});
    }
    else if (kernel.operation == "Index") {
        layout.output_width_ = width;
        // Native Index records fixed dimension descriptors before the B
        // selected row indices. Only data rows use T; selection rows use B.
        expected_inputs = {
            { tokens, width },
            { 1 },
            { 2 },
            { requests }
        };
        expected_outputs = {
            { requests, width }
        };
        layout.input_token_axes_ = {
            { 0, 1 },
            { 3, 1, AxisExtent::Requests }
        };
    }
    else return std::nullopt;

    if (KernelShapes(kernel.input_shapes).tensors != expected_inputs || KernelShapes(kernel.output_shapes).tensors != expected_outputs) return std::nullopt;
    return layout;
}

std::optional<ModelKernelLayout> ModelKernelLayout::bind_embedding(const SourceNativeRegion & region, const SourceKernel & kernel, std::int64_t tokens) {
    const auto & ids = region.inputs.at("input_");
    if (!ids || ids->shape != std::vector<std::int64_t>{ tokens }) return std::nullopt;
    const auto hidden = region.parameters.at("hidden_size");
    const KernelShapes inputs(kernel.input_shapes);
    const KernelShapes outputs(kernel.output_shapes);

    if (kernel.operation == "GatherV2") {
        if (inputs.tensors.size() != 3 || inputs.tensors[0].size() != 2 || inputs.tensors[0][1] != hidden) return std::nullopt;
        const std::vector<std::vector<std::int64_t>> expected_inputs{ inputs.tensors[0], { tokens }, { 1 } };
        const std::vector<std::vector<std::int64_t>> expected_outputs{
            { tokens, hidden }
        };
        if (inputs.tensors != expected_inputs || outputs.tensors != expected_outputs) return std::nullopt;

        // Weight rows belong to the fixed native TP shard. Only token IDs
        // and gathered activations vary with this invocation.
        ModelKernelLayout layout(ModelKernelRole::ShapeOnly, hidden);
        layout.input_token_axes_ = {
            { 1, 1 }
        };
        return layout;
    }
    if (kernel.operation == "MaskedFill") {
        const std::vector<std::vector<std::int64_t>> expected_inputs{
            { tokens, hidden },
            { tokens, 1 },
            {}
        };
        const std::vector<std::vector<std::int64_t>> expected_outputs{
            { tokens, hidden }
        };
        if (inputs.tensors != expected_inputs || outputs.tensors != expected_outputs) return std::nullopt;

        ModelKernelLayout layout(ModelKernelRole::ShapeOnly, hidden);
        layout.input_token_axes_ = {
            { 0, 1 },
            { 1, 1 }
        };
        return layout;
    }

    // The native masking expression uses token vectors and Python scalar
    // bounds. Explicit operand forms distinguish scalars from T=1 vectors.
    std::vector<std::vector<std::int64_t>> expected_inputs{ { tokens } };
    bool second_vector = false;
    if (kernel.operation == "GreaterEqual" || kernel.operation == "Less") expected_inputs.push_back({});
    else if (kernel.operation == "Add" || kernel.operation == "Sub" || kernel.operation == "LogicalAnd" || kernel.operation == "LogicalOr") {
        expected_inputs.push_back({ tokens });
        second_vector = true;
    }
    else if (kernel.operation == "Mul") {
        if (inputs.tensors.size() != 2) return std::nullopt;
        second_vector = !inputs.tensors[1].empty();
        expected_inputs.push_back(second_vector ? std::vector<std::int64_t>{ tokens } : std::vector<std::int64_t>{});
    }
    else if (kernel.operation != "Cast" && kernel.operation != "LogicalNot") return std::nullopt;

    const std::vector<std::vector<std::int64_t>> expected_outputs{ { tokens } };
    if (inputs.tensors != expected_inputs || outputs.tensors != expected_outputs) return std::nullopt;
    ModelKernelLayout layout(ModelKernelRole::ShapeOnly, 1);
    if (second_vector) layout.input_token_axes_.push_back({ 1, 1 });
    return layout;
}

std::optional<ModelKernelLayout> ModelKernelLayout::bind_attention_rows(const SourceNativeRegion & region, const SourceKernel & kernel, std::int64_t tokens) {
    if (kernel.operation != "Cast" && kernel.operation != "Slice") return std::nullopt;

    const auto kv_heads = region.parameters.at("kv_heads");
    const auto head_dim = region.parameters.at("head_dim");
    const auto & value = region.inputs.at("v");
    if (!value || value->shape != std::vector<std::int64_t>{ tokens, kv_heads * head_dim }) return std::nullopt;

    const KernelShapes inputs(kernel.input_shapes);
    const KernelShapes outputs(kernel.output_shapes);
    if (kernel.operation == "Cast") {
        // The non-FIA cache writer converts one destination slot per token
        // from int64 to int32. Slot values are not copied into target state.
        const std::vector<std::vector<std::int64_t>> slots{ { tokens } };
        if (inputs.tensors != slots || outputs.tensors != slots || kernel.input_dtypes != "INT64" || kernel.output_dtypes != "INT32") return std::nullopt;
        return ModelKernelLayout(ModelKernelRole::ShapeOnly, 1);
    }

    // Prefill V is a strided view into QKV. Require the observed row stride,
    // not just a matching output width, before binding its materialization.
    const auto packed_heads = region.parameters.at("query_heads") + 2 * kv_heads;
    if (value->stride != std::vector<std::int64_t>{ packed_heads * head_dim, 1 }) return std::nullopt;
    const std::vector<std::vector<std::int64_t>> expected_inputs{
        { tokens, packed_heads, head_dim },
        { 3 },
        { 3 }
    };
    const std::vector<std::vector<std::int64_t>> expected_outputs{
        { tokens, kv_heads, head_dim }
    };
    if (inputs.tensors != expected_inputs || outputs.tensors != expected_outputs) return std::nullopt;

    return ModelKernelLayout(ModelKernelRole::ShapeOnly, kv_heads * head_dim);
}

std::optional<ModelKernelLayout> ModelKernelLayout::bind_norm(const SourceNativeRegion & region, const SourceKernel & kernel, std::int64_t serving_tokens,
                                                              const SourceNativeRegion * parameter_child) {
    const bool layer = region.semantic == "decoder_layer";
    const bool qkv = region.semantic == "qkv_preparation" && region.method == "forward_prepare_native";
    const bool model = region.semantic == "model_body";
    if (!layer && !qkv && !model) return std::nullopt;

    std::int64_t width = 0;
    if (model) {
        // Qwen3Model inherits the final norm from Qwen2Model. Its hidden width
        // is established by the actual embedding child, not a coincident kernel dimension.
        if (!parameter_child || parameter_child->semantic != "embedding") return std::nullopt;
        const auto & token_ids = parameter_child->inputs.at("input_");
        if (!token_ids || token_ids->shape != std::vector<std::int64_t>{ serving_tokens }) return std::nullopt;
        width = parameter_child->parameters.at("hidden_size");
    }
    else {
        const auto & activation = region.inputs.at("hidden_states");
        if (!activation || activation->shape.size() != 2 || activation->shape[0] != serving_tokens) return std::nullopt;
        width = activation->shape[1];
    }

    const KernelShapes inputs(kernel.input_shapes);
    const KernelShapes outputs(kernel.output_shapes);
    const bool residual = kernel.operation == "AddRmsNorm";
    std::int64_t multiplier = 1;

    if (qkv) {
        if (residual) return std::nullopt;
        width = region.parameters.at("head_dim");
        const auto query_heads = region.parameters.at("query_heads");
        const auto kv_heads = region.parameters.at("kv_heads");
        // apply_qk_norm flattens tokens * heads into rows. Equal Q/K head
        // counts describe the same row relation and need no role distinction.
        if (inputs.tensors[0] == std::vector<std::int64_t>{ serving_tokens * query_heads, width }) { multiplier = query_heads; }
        else if (inputs.tensors[0] == std::vector<std::int64_t>{ serving_tokens * kv_heads, width }) { multiplier = kv_heads; }
        else return std::nullopt;
    }

    const auto rows = serving_tokens * multiplier;
    std::vector<std::vector<std::int64_t>> expected_inputs{
        { rows, width }
    };
    if (residual) expected_inputs.push_back({ rows, width });
    expected_inputs.push_back({ width });
    std::vector<std::vector<std::int64_t>> expected_outputs{
        { rows, width },
        { rows,     1 }
    };
    if (residual) expected_outputs.push_back({ rows, width });
    if (inputs.tensors != expected_inputs || outputs.tensors != expected_outputs) return std::nullopt;

    ModelKernelLayout layout(ModelKernelRole::ShapeOnly, width);
    layout.input_token_axes_ = {
        { 0, multiplier }
    };
    layout.output_token_axes_ = {
        { 0, multiplier }
    };
    // The reciprocal-RMS output and fused residual are real operands, even if
    // Python discards an output. Preserve and parameterize all native slots.
    layout.output_token_axes_.push_back({ 1, multiplier });
    if (residual) {
        layout.input_token_axes_.push_back({ 1, multiplier });
        layout.output_token_axes_.push_back({ 2, multiplier });
    }
    return layout;
}

ModelParameterRule ModelKernelLayout::compile(const SourceKernel & source) const {
    ModelWorkload workload = ModelWorkload::None;
    if (role_ == ModelKernelRole::SequenceLengthMaximum) workload = ModelWorkload::SequenceMaximum;
    else if (role_ == ModelKernelRole::SequenceLengthsCast) workload = ModelWorkload::SequenceLengths;
    else if (role_ == ModelKernelRole::PageTableCast && source.output_dtypes == "INT32") workload = ModelWorkload::PageTable;
    else if (role_ == ModelKernelRole::ForwardPositions || role_ == ModelKernelRole::DecodePositionClamp) workload = ModelWorkload::Positions;
    else if (role_ == ModelKernelRole::SamplingPositions) workload = ModelWorkload::LastPositions;
    else if (role_ == ModelKernelRole::LogitsSelect) workload = ModelWorkload::LogitsIndices;

    return ModelParameterRule(input_token_axes_, output_token_axes_, page_size_tokens_, workload);
}

} // namespace markov
