#include "markov/trace_graph/structure/tensor_program.hpp"

#include <limits>
#include <utility>

namespace markov::trace_graph::structure {
namespace {

/** One eager sequence step: caller preparation, then a queued tensor call.
 * Earlier tensor completion constrains the device, not CPU preparation.
 * The program consists of these alternating steps until its caller return.
 */
void append_eager_tensor_operation(std::vector<TensorOperation> & program, OperationWork work, const std::string & caller) {
    auto preparation = work;
    preparation.operation += ".prepare";
    preparation.implementation = caller;
    preparation.timing_scope = "cpu_service";
    program.push_back({ std::move(preparation), {}, std::nullopt, TensorExecutionKind::CpuService });

    std::vector<size_t> predecessors{ program.size() - 1 };
    if (program.size() > 1) predecessors.push_back(program.size() - 2);

    program.push_back({ std::move(work), std::move(predecessors) });
}

} // namespace

std::string_view tensor_execution_name(TensorExecutionKind kind) {
    switch (kind) {
    case TensorExecutionKind::AsyncDevice:
        return "async_device";
    case TensorExecutionKind::CpuService:
        return "cpu_service";
    case TensorExecutionKind::BlockingContextReadback:
        return "blocking_readback";
    case TensorExecutionKind::DirectBlockingDevice:
        return "direct_blocking_device";
    case TensorExecutionKind::LayerReady:
        return "hicache_layer_ready";
    }
    return "unknown";
}

std::vector<TensorOperation> dense_tensor_program(const std::string & component, const std::string & implementation, const DenseShape & shape) {
    const auto features = std::visit([](const auto & value) { return value.features(); }, shape);
    if (const auto * buffers = std::get_if<InputBuffersShape>(&shape)) {
        // The eager registry groups by destination/source dtype, preserving
        // first occurrence order. Its declared token axes resize with Prefill;
        // request axes remain one element in this single-request path.
        std::map<std::pair<std::string, std::string>, size_t> indices;
        std::vector<OperationWork> groups;
        for (const auto & copy : buffers->copies) {
            const auto key = std::pair{ copy.destination_dtype, copy.source_dtype };
            const auto [at, added] = indices.emplace(key, groups.size());
            if (added) {
                groups.push_back({
                    component,
                    component + ".input_buffers.copy",
                    "torch._foreach_copy_:" + copy.source_dtype + ":" + copy.destination_dtype,
                    "device_service",
                    { { "tensor_count", 0.0 }, { "elements", 0.0 } }
                });
            }
            auto & group = groups[at->second];
            group.features.at("tensor_count") += 1.0;
            group.features.at("elements") += static_cast<double>(copy.token_axis ? buffers->rows : 1);
        }

        std::vector<TensorOperation> program;
        for (auto & group : groups) {
            // Entry builds the registry copy lists; later groups only advance
            // the dtype-group loop. These are different host operations, not
            // one cost fitted indirectly through their tensor-count difference.
            const bool entry = program.empty();
            program.push_back({
                { component,
                 component + (entry ? ".input_buffers.entry" : ".input_buffers.next_group"),
                 "sglang.EagerRunner.load_batch", "cpu_service",
                 entry ? features : group.features },
                {},
                std::nullopt,
                TensorExecutionKind::CpuService
            });
            program.push_back({ std::move(group), { program.size() - 1 } });
        }
        program.push_back({
            { component, component + ".input_buffers.return", "sglang.EagerRunner.load_batch", "cpu_service", features },
            {},
            std::nullopt,
            TensorExecutionKind::CpuService
        });
        return program;
    }
    if (std::holds_alternative<LogitsPruningShape>(shape)) {
        const auto operation = component + ".logits_pruning";
        const std::map<std::string, double> index_features{
            { "elements", 1.0 }
        };
        const auto cpu = [&](const std::string & role) {
            return TensorOperation{
                { component, operation + "." + role, "sglang.LogitsProcessor._get_pruned_states", "cpu_service", features },
                {},
                std::nullopt,
                TensorExecutionKind::CpuService
            };
        };
        TensorOperation cumsum{
            { component, operation + ".cumsum", "aten.cumsum:torch.int32:dim=0:torch.int64", "device_service", index_features },
            { 0 }
        };
        // Torch promotes int32 lengths before cumsum. Both device launches
        // belong to one queued call; do not add a second CPU submission.
        cumsum.device_launches = {
            { component, operation + ".length_cast", "ascend.cast.int32_to_int64", "device_service", index_features },
            { component,  operation + ".length_sum",  "ascend.cumsum.int64.aicpu", "device_service", index_features }
        };
        return {
            cpu("prepare_lengths"),
            std::move(cumsum),
            cpu("prepare_last_index"),
            {      { component, operation + ".last_index", "aten.sub:torch.int64:scalar", "device_service", index_features }, { 2, 1 } },
            cpu("prepare_selection"),
            { { component, operation + ".select", "aten.index:logits_hidden:" + implementation, "device_service", features }, { 4, 3 } },
            cpu("return")
        };
    }
    if (const auto * sampling = std::get_if<GreedySamplingShape>(&shape)) {
        // Own only the inner Torch argmax, not the enclosing sampler.
        TensorOperation call{
            { component, component + ".greedy_argmax", implementation, "device_service", features },
            {}
        };
        if (implementation == "aten.argmax:torch.float32:dim=-1:ascend910b" && sampling->rows == 1
            && sampling->vocabulary_width <= static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
            // 910B ArgMaxV2 produces int32 indices. The aclnn API then casts
            // those indices to Torch's int64 output, within the same queued
            // call. Cast work scales with output rows, not vocabulary size.
            call.device_launches.push_back({ component, call.work.operation + ".reduce", "ascend.argmax.float32_to_int32.aicore", "device_service", features });
            const std::map<std::string, double> index_features{
                { "elements", static_cast<double>(sampling->rows) }
            };
            call.device_launches.push_back({ component, call.work.operation + ".cast", "ascend.cast.int32_to_int64", "device_service", index_features });
        }
        return { std::move(call) };
    }
    if (std::holds_alternative<EmbeddingOutputMaskShape>(shape)) {
        // Ownership is the inner Torch call, not the enclosing embedding
        // forward. Its own submission/return scopes already cover host work.
        return {
            { { component, component + ".embedding_output_mask", implementation, "device_service", features }, {} }
        };
    }
    if (const auto * norm = std::get_if<NormalizationShape>(&shape); norm && norm->split_residual) {
        const auto suffix = implementation.substr(implementation.find(':') + 1);
        const auto dtype = suffix.substr(0, suffix.find(':'));
        const auto operation = component + ".split_add_rms_norm";
        const auto cpu = [&](const std::string & role) {
            return TensorOperation{
                { component, operation + "." + role, "sglang.RMSNorm.forward_npu", "cpu_service", features },
                {},
                std::nullopt,
                TensorExecutionKind::CpuService
            };
        };
        auto add_features = features;
        add_features.erase("epsilon");
        auto norm_features = features;
        // The out-of-place tensor addition produces packed storage. Its
        // output, not the original residual view, is the RMSNorm input.
        norm_features["input_stride_0"] = static_cast<double>(norm->width);
        norm_features["input_stride_1"] = 1.0;
        return {
            cpu("entry"),        {                  { component, operation + ".add", "aten.add:" + dtype, "device_service", add_features },    { 0 } },
            cpu("norm_prepare"), { { component, component + ".rms_norm", "torch_npu.rms_norm:" + suffix, "device_service", norm_features }, { 2, 1 } },
            cpu("return"),
        };
    }
    if (std::holds_alternative<EmbeddingMaskShape>(shape)) {
        std::vector<TensorOperation> program;
        // This function is explicitly eager on NPU. Comparisons produce bool
        // masks; scalar offsets and the final remapping produce int64 indices.
        for (const auto & [role, backend] : {
                 std::pair{       "org_lower",          "aten.ge" },
                 {       "org_upper",          "aten.lt" },
                 {        "org_mask", "aten.bitwise_and" },
                 {     "added_lower",          "aten.ge" },
                 {     "added_upper",          "aten.lt" },
                 {      "added_mask", "aten.bitwise_and" },
                 {      "org_offset",         "aten.mul" },
                 {    "added_offset",         "aten.mul" },
                 {    "valid_offset",         "aten.add" },
                 {      "vocab_mask",  "aten.bitwise_or" },
                 { "subtract_offset",         "aten.sub" },
                 {   "remap_indices",         "aten.mul" },
                 {    "invalid_mask", "aten.bitwise_not" }
        }) {
            const auto operation = component + ".embedding_mask." + role;
            append_eager_tensor_operation(program,
                                          { component, operation, std::string(backend) + ":" + implementation, "device_service", features },
                                          "sglang.get_masked_input_and_mask.npu_eager");
            if (std::string_view(backend) == "aten.mul") {
                // Eager NPU embedding offsets/remapping multiply a bool mask
                // by an int64 scalar or tensor. The mask promotion and the
                // multiplication are distinct launches in the same task.
                program.back().device_launches = {
                    { component,  operation + ".promote", "ascend.cast.bool_to_int64", "device_service", features },
                    { component, operation + ".multiply",   "ascend.mul.int64.aicore", "device_service", features },
                };
            }
        }
        program.push_back({
            { component, component + ".embedding_mask.return", "sglang.get_masked_input_and_mask.npu_eager", "cpu_service", features },
            {},
            std::nullopt,
            TensorExecutionKind::CpuService
        });
        return program;
    }
    if (const auto * rotary = std::get_if<RotaryShape>(&shape); rotary && rotary->native_float_table) {
        std::vector<TensorOperation> program;
        const auto append = [&](const std::string & role, const std::string & backend, uint64_t width) {
            auto work = features;
            work["output_width"] = static_cast<double>(width);
            work["output_elements"] = static_cast<double>(rotary->rows) * static_cast<double>(width);
            const auto operation = component + ".rotary.native." + role;
            append_eager_tensor_operation(program,
                                          { component, operation, backend + ":" + implementation, "device_service", std::move(work) },
                                          "sglang.RotaryEmbedding.forward_npu");
        };

        // forward_npu eagerly gathers before selecting forward_native; the
        // fallback gathers again. The unused first result is still real work.
        append("dispatch_gather", "aten.index_select", rotary->head_dim);
        append("native_gather", "aten.index_select", rotary->head_dim);
        for (const auto & [role, width] : {
                 std::pair{ "query", rotary->query_width },
                 std::pair{   "key",    rotary->kv_width }
        }) {
            const std::string prefix(role);
            append(prefix + ".cos_cast", "aten._to_copy", rotary->head_dim / 2);
            append(prefix + ".sin_cast", "aten._to_copy", rotary->head_dim / 2);
            append(prefix + ".x1_cos", "aten.mul", width / 2);
            append(prefix + ".x2_sin", "aten.mul", width / 2);
            append(prefix + ".subtract", "aten.sub", width / 2);
            append(prefix + ".x2_cos", "aten.mul", width / 2);
            append(prefix + ".x1_sin", "aten.mul", width / 2);
            append(prefix + ".add", "aten.add", width / 2);
            append(prefix + ".rotate_join", rotary->neox_style ? "aten.cat" : "aten.stack", width);
            append(prefix + ".output_join", "aten.cat", width);
        }
        program.push_back({
            { component, component + ".rotary.native.return", "sglang.RotaryEmbedding.forward_npu", "cpu_service", features },
            {},
            std::nullopt,
            TensorExecutionKind::CpuService
        });
        return program;
    }
    if (const auto * table = std::get_if<RotaryTableShape>(&shape)) {
        std::vector<TensorOperation> program;
        const auto append = [&](const std::string & role, const std::string & backend) {
            const auto operation = component + ".rotary_table." + role;
            append_eager_tensor_operation(program,
                                          { component, operation, backend + ":" + implementation, "device_service", features },
                                          "sglang.RotaryEmbedding.get_cos_sin_with_position");
        };
        append("gather", "aten.index_select");
        append("repeat", "aten.repeat");
        // Chunked views have row stride 2*width. A singleton row is already
        // contiguous; otherwise each output materializes its own packed copy.
        if (table->rows > 1) {
            append("cos_copy", "aten.clone");
            append("sin_copy", "aten.clone");
        }
        program.push_back({
            { component, component + ".rotary_table.return", "sglang.RotaryEmbedding.get_cos_sin_with_position", "cpu_service", features },
            {},
            std::nullopt,
            TensorExecutionKind::CpuService
        });
        return program;
    }
    std::string operation, caller;
    if (std::holds_alternative<LinearShape>(shape)) {
        operation = component + ".linear";
        caller = implementation.starts_with("torch.matmul.logits:") ? "sglang.LogitsProcessor._compute_lm_head" : "sglang.UnquantizedLinearMethod.apply";
    }
    else if (std::holds_alternative<GatedActivationShape>(shape)) {
        operation = component + ".gated_activation";
        caller = "sglang.SiluAndMul.forward_npu";
    }
    else if (std::holds_alternative<EmbeddingShape>(shape)) {
        operation = component + ".embedding_lookup";
        caller = "sglang.UnquantizedEmbeddingMethod.embedding";
    }
    else if (std::holds_alternative<LogitsOutputShape>(shape)) {
        operation = component + ".logits_output";
        caller = "sglang.LogitsProcessor._copy_logits_to_buffer";
    }
    else if (std::holds_alternative<NormalizationShape>(shape)) {
        operation = component + (std::get<NormalizationShape>(shape).residual ? ".add_rms_norm" : ".rms_norm");
        caller = "sglang.RMSNorm.forward_npu";
    }
    else if (std::holds_alternative<QkvRopeShape>(shape)) {
        operation = component + ".qkv_norm_rope";
        caller = "sgl_kernel_npu.split_qkv_rmsnorm_rope";
    }
    else if (std::holds_alternative<RotaryShape>(shape)) {
        operation = component + ".rotary";
        caller = "sglang.RotaryEmbedding.forward_npu";
    }
    else {
        operation = component + ".qk_reshape";
        caller = "sglang.reshape_for_qk_norm";
    }
    const auto cpu = [&](const std::string & role) {
        return TensorOperation{
            { component, operation + "." + role, caller, "cpu_service", features },
            {},
            std::nullopt,
            TensorExecutionKind::CpuService
        };
    };
    if (const auto * reshape = std::get_if<QkReshapeShape>(&shape); reshape && !reshape->copies_input()) return { cpu("view") };
    if (const auto * logits = std::get_if<LogitsOutputShape>(&shape); logits && !logits->copies_input) return { cpu("view") };
    const auto * qkv = std::get_if<QkvRopeShape>(&shape);
    const auto execution = qkv && !qkv->task_queue ? TensorExecutionKind::DirectBlockingDevice : TensorExecutionKind::AsyncDevice;
    return {
        cpu("entry"),
        { { component, operation, implementation, "device_service", features }, { 0 }, std::nullopt, execution },
        cpu("return")
    };
}

} // namespace markov::trace_graph::structure
