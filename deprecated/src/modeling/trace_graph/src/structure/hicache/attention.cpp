#include "markov/trace_graph/structure/hicache/attention.hpp"
#include "markov/trace_graph/core/numeric.hpp"

#include <map>
#include <utility>

namespace markov::trace_graph::structure::hicache {

std::optional<TensorOperation> ascend_page_table_division(const std::string & component, uint64_t context_tokens, uint64_t page_tokens,
                                                          std::string_view device_name) {
    if (!device_name.starts_with("Ascend910B")) return std::nullopt;

    const auto columns = context_tokens / page_tokens + static_cast<uint64_t>(context_tokens % page_tokens != 0);
    const std::map<std::string, double> features{
        {         "elements",        static_cast<double>(columns) },
        {   "input_elements", static_cast<double>(context_tokens) },
        { "page_size_tokens",    static_cast<double>(page_tokens) },
        {       "batch_size",                                   1 },
    };
    TensorOperation result{
        { component, component + ".metadata.page_division", "aten.floor_divide:page_table_int32", "device_service", features },
        {}
    };
    const auto append = [&](const std::string & role, const std::string & implementation) {
        result.device_launches.push_back({ component, result.work.operation + "." + role, implementation, "device_service", features });
    };

    // With one request, the view reduces to [columns] at stride page_tokens.
    // CANN's Slice optimization views storage as [floor(context/page), page]
    // and selects [columns, 1]. The bounds check fails on a partial last page,
    // selecting AsStrided instead. Singleton dimensions need no materialization.
    if (page_tokens > 1 && columns > 1)
        append("materialize", context_tokens % page_tokens == 0 ? "ascend.slice.int32.aicore" : "ascend.as_strided.int32.aicore");
    append("promote", "ascend.cast.int32_to_int64");
    append("divide", "ascend.floor_divide.int64.aicore");
    append("output", "ascend.cast.int64_to_int32");
    return result;
}

std::optional<std::vector<TensorOperation>> ascend_attention_metadata_program(const std::string & component, const AttentionMetadataInputs & input) {
    auto division = ascend_page_table_division(component, input.context_tokens, input.page_tokens, input.device_name);
    if (!division) return std::nullopt;

    const auto columns = input.context_tokens / input.page_tokens + static_cast<uint64_t>(input.context_tokens % input.page_tokens != 0);
    const auto capacity = input.max_context_tokens / input.page_tokens + static_cast<uint64_t>(input.max_context_tokens % input.page_tokens != 0);
    const std::map<std::string, double> shape{
        {                  "batch_size",                                         1 },
        {              "context_tokens", static_cast<double>(input.context_tokens) },
        {            "page_size_tokens",    static_cast<double>(input.page_tokens) },
        {          "page_table_columns",              static_cast<double>(columns) },
        { "page_table_capacity_columns",             static_cast<double>(capacity) },
    };
    std::vector<TensorOperation> program;
    const auto append = [&](TensorOperation operation) {
        auto preparation = operation.work;
        preparation.operation = program.empty() ? component + ".metadata.entry" : operation.work.operation + ".prepare";
        preparation.implementation = input.graph ? "sglang.AscendAttnBackend.metadata.graph" : "sglang.AscendAttnBackend.metadata.eager";
        preparation.timing_scope = "cpu_service";
        preparation.features = shape;
        program.push_back({ std::move(preparation), {}, std::nullopt, TensorExecutionKind::CpuService });

        operation.predecessors = { program.size() - 1 };
        if (program.size() > 1) operation.predecessors.push_back(program.size() - 2);
        program.push_back(std::move(operation));
    };
    const auto tensor =
        [&](const std::string & role, const std::string & implementation, uint64_t elements, TensorExecutionKind execution = TensorExecutionKind::AsyncDevice) {
            auto features = shape;
            features.emplace("elements", static_cast<double>(elements));
            append({
                { component, component + ".metadata." + role, implementation, "device_service", std::move(features) },
                {},
                std::nullopt,
                execution
            });
        };

    // Eager slicing converts the device maximum into a Python index. Graph
    // replay uses seq_lens_cpu instead; its max/item belongs to CPU entry.
    if (!input.graph) {
        tensor("context_max", "aten.max:" + input.sequence_dtype, 1);
        tensor("context_readback", "aten._local_scalar_dense:" + input.sequence_dtype, 1, TensorExecutionKind::BlockingContextReadback);
    }
    auto index_features = shape;
    index_features.emplace("elements", static_cast<double>(input.context_tokens));
    index_features.emplace("table_rows", static_cast<double>(input.request_table_rows));
    index_features.emplace("table_columns", static_cast<double>(input.request_table_columns));
    TensorOperation indices{
        { component, component + ".metadata.token_indices", "aten.index:page_table_int32", "device_service", index_features },
        {}
    };

    // req_to_token[:, :context] retains the full storage row stride. With
    // multiple rows and a shortened width, aclnnIndex materializes this view
    // before gathering the requested row. These are separate worker launches,
    // not one device cost followed by a fictitious idle interval.
    if (input.request_table_rows > 1 && input.context_tokens < input.request_table_columns) {
        auto slice_features = index_features;
        slice_features["elements"] =
            static_cast<double>(core::checked_multiply_u64(input.request_table_rows, input.context_tokens, "Request table slice size overflow"));
        indices.device_launches.push_back(
            { component, indices.work.operation + ".materialize", "ascend.slice.int32.aicore", "device_service", std::move(slice_features) });
    }
    indices.device_launches.push_back({ component, indices.work.operation + ".gather", "ascend.index.int32.aicore", "device_service", index_features });
    append(std::move(indices));
    append(std::move(*division));

    if (input.graph) {
        tensor("table_copy", "aten.copy_", columns);
        // graph_metadata[bs] is already a [:bs] view. For one real request
        // the trailing-row fill has no device elements. Its CPU call remains
        // in preparation. The trailing-column fill is real work when nonempty.
        if (capacity > columns) tensor("table_clear", "aten.fill_:int32", capacity - columns);
        if (!input.sequence_copy_identity) tensor("sequence_copy", "aten.copy_", 1);
    }
    else {
        if (input.extend_readback_dtype)
            tensor("extend_readback", "aten.to.cpu:" + *input.extend_readback_dtype, 1, TensorExecutionKind::BlockingContextReadback);
        if (input.sequence_dtype != "torch.int32") tensor("sequence_cast", "aten._to_copy:" + input.sequence_dtype + "->torch.int32", 1);
    }

    // CPU conversions, numpy cumsum and assignment remain in the observed
    // inter-call/return scopes, not invented device kernels or zero costs.
    program.push_back({
        { component,
         component + ".metadata.return",
         input.graph ? "sglang.AscendAttnBackend.metadata.graph" : "sglang.AscendAttnBackend.metadata.eager",
         "cpu_service", shape },
        {},
        std::nullopt,
        TensorExecutionKind::CpuService
    });
    return program;
}

std::optional<AttentionInputs> target_eager_attention_inputs(const AscendAttentionConditions & conditions, const AttentionBatchShape & source_batch,
                                                             const AttentionCacheWrite & source_write, const AttentionEagerPadding & padding,
                                                             std::span<const AttentionRequestShape> target_requests, uint64_t target_page_tokens) {
    if (conditions.mla || conditions.context_parallel || conditions.encoder || conditions.graph || padding.mlp_sync) return std::nullopt;

    AttentionInputs result{ source_batch, source_write };
    result.batch.requests.assign(target_requests.begin(), target_requests.end());
    result.batch.page_tokens = target_page_tokens;
    uint64_t tokens = 0;
    for (const auto & request : target_requests) tokens = core::checked_add_u64(tokens, request.query_tokens, "Target attention token count overflow");

    result.batch.padded_query_tokens = tokens;
    if (conditions.prefill && !conditions.verify && padding.allow_input_scattered && !padding.can_run_tbo) {
        const auto remainder = tokens % padding.tp_size;
        if (remainder) result.batch.padded_query_tokens = core::checked_add_u64(tokens, padding.tp_size - remainder, "Target attention padding overflow");
    }
    if (!source_write.enabled) return result;

    const auto key_width = core::checked_multiply_u64(source_batch.kv_heads, conditions.query_key_dim, "Attention key width overflow");
    const auto value_width = core::checked_multiply_u64(source_batch.kv_heads, conditions.value_dim, "Attention value width overflow");
    // Packed ordinary MHA K/V has one row per padded input token. A different
    // observed layout is not permission to extrapolate element-count ratios.
    if (source_write.locations != source_batch.padded_query_tokens
        || source_write.key_elements != core::checked_multiply_u64(source_write.locations, key_width, "Source key size overflow")
        || source_write.value_elements != core::checked_multiply_u64(source_write.locations, value_width, "Source value size overflow"))
        return std::nullopt;

    result.cache_write.locations = result.batch.padded_query_tokens;
    result.cache_write.key_elements = core::checked_multiply_u64(result.cache_write.locations, key_width, "Target key size overflow");
    result.cache_write.value_elements = core::checked_multiply_u64(result.cache_write.locations, value_width, "Target value size overflow");
    return result;
}

std::optional<AttentionDispatch> ascend_attention_dispatch(const AscendAttentionConditions & input) {
    // Preserve the order of forward_extend/forward_decode dispatch. Graph
    // decode uses FIA even when the eager use_fia switch is off.
    if (input.prefill && input.dllm) return AttentionDispatch{ "dllm" };
    if (input.sparse) return AttentionDispatch{ "sparse" };
    if (input.prefill && (input.verify || input.draft)) return AttentionDispatch{ "mtp" };
    if (!input.prefill && input.graph && !input.compile) {
        if (input.sinks || input.hybrid_swa) return AttentionDispatch{ input.fia ? "decode_graph.sinks_fia" : "decode_graph.sinks_triton" };
        if (input.mla) return AttentionDispatch{ "decode_graph.mla" };
        return AttentionDispatch{ "decode_graph.fia", "BSH" };
    }
    if (input.mla) return AttentionDispatch{ "mla" };

    const bool tnd = (input.query_key_dim == input.value_dim && (input.query_key_dim == 128 || input.query_key_dim == 192 || input.query_key_dim == 256))
                     || (input.query_key_dim == 192 && input.value_dim == 128);
    if (input.sinks || (input.swa_layer && input.fia)) return AttentionDispatch{ input.fia ? "sinks_fia" : "sinks_triton" };
    if (input.prefill && input.context_parallel) return AttentionDispatch{ input.fia ? "context_parallel_fia" : "framework_rejects_non_fia_context_parallel" };
    if (input.fia) return AttentionDispatch{ "fia", input.prefill && tnd ? "TND" : "BSND", input.prefill && !tnd };

    const bool native = input.encoder || input.logit_cap != 0 || (input.prefill && (input.query_key_dim > 128 || !input.causal || input.native_sdpa));
    if (native) return AttentionDispatch{ "native_sdpa" };
    if (!input.alibi) return std::nullopt;
    if (*input.alibi) return AttentionDispatch{ "alibi" };
    return AttentionDispatch{ input.prefill ? "flash_attention_qlens" : "paged_attention" };
}

namespace {

std::optional<std::vector<TensorOperation>> attention_score_program(const AscendAttentionConditions & conditions, const AttentionBatchShape & batch) {
    const auto dispatch = ascend_attention_dispatch(conditions);
    if (!dispatch) return std::nullopt;
    const bool eager_fia = dispatch->branch == "fia";
    const bool graph_fia = dispatch->branch == "decode_graph.fia";
    const bool flash = dispatch->branch == "flash_attention_qlens";
    const bool paged = dispatch->branch == "paged_attention";
    if (!eager_fia && !graph_fia && !flash && !paged) return std::nullopt;

    std::vector<TensorOperation> program;
    const std::string component = conditions.prefill ? "prefill" : "decode";
    const auto append = [&](const std::string & operation,
                            const std::string & implementation,
                            std::map<std::string, double>
                                features,
                            std::vector<size_t>
                                predecessors,
                            std::optional<size_t> request_index = std::nullopt) {
        features.emplace("query_heads", static_cast<double>(batch.query_heads));
        features.emplace("kv_heads", static_cast<double>(batch.kv_heads));
        features.emplace("query_key_dim", static_cast<double>(conditions.query_key_dim));
        features.emplace("value_dim", static_cast<double>(conditions.value_dim));
        features.emplace("page_tokens", static_cast<double>(batch.page_tokens));
        program.push_back({
            OperationWork{ component, component + ".attention." + operation, implementation, "device_service", std::move(features) },
            std::move(predecessors),
            request_index
        });
        return program.size() - 1;
    };

    uint64_t query_tokens = 0;
    uint64_t context_tokens = 0;
    double attention_pairs = 0;
    for (const auto & request : batch.requests) {
        query_tokens += request.query_tokens;
        context_tokens += request.context_tokens;
        const double query = static_cast<double>(request.query_tokens);
        attention_pairs += query * static_cast<double>(request.context_tokens) - query * (query - 1) / 2;
    }

    if (dispatch->per_request) {
        // Ascend's BSND prefill loop skips empty requests, launches FIA v2,
        // then copies each result into its disjoint output slice. Submission
        // order is retained here; it is not inferred from measured durations.
        std::vector<size_t> previous;
        if (!conditions.sequence_lengths_on_cpu) return std::nullopt;
        if (!*conditions.sequence_lengths_on_cpu) {
            program.push_back({
                OperationWork{ component,
                              component + ".attention.context_readback",
                              "aten.to.cpu", "device_service",
                              { { "elements", static_cast<double>(batch.requests.size()) } } },
                {},
                std::nullopt,
                TensorExecutionKind::BlockingContextReadback
            });
            previous = { program.size() - 1 };
        }
        // tolist and argument construction belong to the following score's
        // CPU preparation interval. Only an actual device readback needs a
        // separate dependency; do not invent another unobservable boundary.

        for (size_t i = 0; i < batch.requests.size(); ++i) {
            const auto & request = batch.requests[i];
            if (request.query_tokens == 0) continue;

            const double query = static_cast<double>(request.query_tokens);
            const double context = static_cast<double>(request.context_tokens);
            const auto score = append("score",
                                      "ascend.fia_v2.BSND",
                                      {
                                          {    "query_tokens",                                     query },
                                          {  "context_tokens",                                   context },
                                          {      "batch_size",                                         1 },
                                          { "attention_pairs", query * context - query * (query - 1) / 2 }
            },
                                      previous,
                                      i);
            const auto copy = append("output_copy",
                                     "aten.copy_",
                                     {
                                         { "output_elements", query * batch.query_heads * conditions.value_dim }
            },
                                     { score },
                                     i);
            previous = { copy };
        }
        return program;
    }

    std::string implementation;
    if (graph_fia) implementation = "ascend.fia.out.BSH";
    else if (flash) implementation = "ascend.flash_attention_qlens";
    else if (paged) implementation = "ascend.paged_attention";
    else {
        // Ordinary eager decode calls torch.ops, which SGLang can replace
        // with its batch-invariant extension. Prefill TND and graph .out
        // use torch_npu entry points and are not changed by that assignment.
        if (!conditions.prefill && !conditions.torch_ops_fia_batch_invariant) return std::nullopt;
        const bool invariant = !conditions.prefill && *conditions.torch_ops_fia_batch_invariant;
        implementation = std::string(invariant ? "ascend.fia.batch_invariant." : "ascend.fia.") + dispatch->layout;
    }

    const auto score = append("score",
                              implementation,
                              {
                                  {        "query_tokens",                                         static_cast<double>(query_tokens) },
                                  {      "context_tokens",                                       static_cast<double>(context_tokens) },
                                  {          "batch_size",                                static_cast<double>(batch.requests.size()) },
                                  {     "attention_pairs",                                                           attention_pairs },
                                  { "tensor_query_tokens", static_cast<double>(eager_fia ? query_tokens : batch.padded_query_tokens) }
    },
                              {});

    if (eager_fia && batch.padded_query_tokens > query_tokens) {
        // The framework restores the padded leading dimension with new_zeros
        // and cat. These are real work, unlike the preceding views/slices.
        const double tail = static_cast<double>(batch.padded_query_tokens - query_tokens);
        const auto zeros = append("output_padding",
                                  "aten.zero_",
                                  {
                                      { "output_elements", tail * batch.query_heads * conditions.value_dim }
        },
                                  { score });
        append("output_concat",
               "aten.cat",
               {
                   { "output_elements", static_cast<double>(batch.padded_query_tokens) * batch.query_heads * conditions.value_dim }
        },
               { score, zeros });
    }
    return program;
}

std::optional<std::vector<TensorOperation>> attention_cache_program(const AscendAttentionConditions & conditions, const AttentionCacheWrite & write) {
    if (!write.enabled) return std::vector<TensorOperation>{};
    if (conditions.mla || conditions.context_parallel) return std::nullopt;

    const std::string component = conditions.prefill ? "prefill" : "decode";
    std::vector<TensorOperation> program;
    const auto append = [&](const std::string & operation, const std::string & implementation, uint64_t elements) {
        std::vector<size_t> predecessors;
        if (!program.empty()) predecessors.push_back(program.size() - 1);
        program.push_back({
            OperationWork{ component,
                          component + ".attention." + operation,
                          implementation, "device_service",
                          { { "elements", static_cast<double>(elements) }, { "locations", static_cast<double>(write.locations) } } },
            std::move(predecessors),
            std::nullopt
        });
    };

    // set_kv_buffer gates both conversions on the key dtype. Tensor.view for
    // the pool's storage dtype is a view, not another device conversion.
    if (write.key_dtype != write.cache_dtype) {
        append("key_cast", "aten._to_copy:" + write.key_dtype + "->" + write.cache_dtype, write.key_elements);
        if (write.value_dtype != write.cache_dtype) append("value_cast", "aten._to_copy:" + write.value_dtype + "->" + write.cache_dtype, write.value_elements);
    }

    if (write.fia) {
        // Only packed inner dimensions with a known token-row layout are
        // covered. Casts can change storage layout and require their own rule.
        if (!write.key_row_stride || !write.value_row_stride || write.locations == 0 || write.key_dtype != write.cache_dtype
            || write.value_dtype != write.cache_dtype)
            return std::nullopt;

        const auto scatter = [&](const std::string & role, uint64_t elements, uint64_t row_stride) {
            append(role, "ascend.scatter_nd_update_", elements);
            if (write.locations <= 1 || row_stride == elements / write.locations) return true;
            if (write.cache_dtype != "torch.bfloat16") return false;

            auto & operation = program.back();
            auto features = operation.work.features;
            features.emplace("source_row_stride", static_cast<double>(row_stride));
            // The Ascend scatter materializes a strided update view before
            // writing it. Both launches belong to the same worker task.
            operation.device_launches.push_back(
                { component, operation.work.operation + ".materialize", "ascend.slice.bfloat16.aicore", "device_service", features });
            operation.device_launches.push_back(
                { component, operation.work.operation + ".scatter", "ascend.scatter_nd_update.bfloat16.aicore", "device_service", features });
            return true;
        };
        if (!scatter("key_write", write.key_elements, *write.key_row_stride) || !scatter("value_write", write.value_elements, *write.value_row_stride))
            return std::nullopt;
    }
    else {
        if (write.location_dtype != "torch.int32") append("location_cast", "aten._to_copy:" + write.location_dtype + "->torch.int32", write.locations);
        append("kv_write", "ascend.reshape_and_cache", write.key_elements + write.value_elements);
    }
    return program;
}

} // namespace

std::optional<std::vector<TensorOperation>> ascend_attention_tensor_program(const AscendAttentionConditions & conditions, const AttentionBatchShape & batch,
                                                                            const AttentionCacheWrite & write) {
    // Getter side effects also belong to the cache implementation, including
    // when this call has save_kv_cache=false and therefore performs no writes.
    if (conditions.cache_pool_implementation != "sglang.srt.hardware_backend.npu.memory_pool_npu.NPUMHATokenToKVPool") return std::nullopt;

    auto score = attention_score_program(conditions, batch);
    auto cache = attention_cache_program(conditions, write);
    if (!score || !cache) return std::nullopt;

    if (!conditions.layer_counter_registered) return std::nullopt;
    if (*conditions.layer_counter_registered) {
        if (!conditions.cache_layer_index) return std::nullopt;
        // Ordinary NPUMHA calls get_key_buffer and get_value_buffer after
        // writing K/V. Both getters check the same layer completion. Keep two
        // control calls even when the target check becomes an inactive return.
        for (size_t getter = 0; getter < 2; ++getter) {
            std::vector<size_t> predecessors;
            if (!cache->empty()) predecessors.push_back(cache->size() - 1);
            cache->push_back({
                OperationWork{ "hicache_io_control", "layer_wait.call", "sglang.LayerDoneCounter.wait_until", "cpu_service", {} },
                std::move(predecessors),
                std::nullopt,
                TensorExecutionKind::LayerReady,
                conditions.cache_layer_index
            });
        }
    }

    const auto offset = cache->size();
    for (auto & operation : *score) {
        for (auto & predecessor : operation.predecessors) predecessor += offset;
        if (offset && operation.predecessors.empty()) operation.predecessors.push_back(offset - 1);

        cache->push_back(std::move(operation));
    }

    // Host work between calls includes dispatch, views and argument assembly.
    // Place it before its actual consumer, not all at the outer entry/return:
    // that placement preserves CPU/device overlap and layer-check ordering.
    const std::string component = conditions.prefill ? "prefill" : "decode";
    const std::map<std::string, double> host_features{
        {          "batch_size",     static_cast<double>(batch.requests.size()) },
        { "tensor_query_tokens", static_cast<double>(batch.padded_query_tokens) }
    };
    std::vector<TensorOperation> program;
    std::vector<size_t> execution_indices;
    for (auto & operation : *cache) {
        auto preparation = operation.work;
        preparation.component = component;
        preparation.operation = program.empty() ? component + ".attention.entry" : operation.work.operation + ".prepare";
        preparation.implementation = "sglang.AscendAttnBackend";
        preparation.timing_scope = "cpu_service";
        preparation.features.emplace("call_batch_size", static_cast<double>(batch.requests.size()));
        preparation.features.emplace("call_query_tokens", static_cast<double>(batch.padded_query_tokens));
        program.push_back({ std::move(preparation), {}, operation.request_index, TensorExecutionKind::CpuService });

        for (auto & predecessor : operation.predecessors) predecessor = execution_indices.at(predecessor);
        operation.predecessors.push_back(program.size() - 1);
        execution_indices.push_back(program.size());
        program.push_back(std::move(operation));
    }
    // Returning the output tensor does not synchronize its device service.
    program.push_back({
        OperationWork{ component, component + ".attention.return", "sglang.AscendAttnBackend", "cpu_service", host_features },
        {},
        std::nullopt,
        TensorExecutionKind::CpuService
    });
    return program;
}

} // namespace markov::trace_graph::structure::hicache
