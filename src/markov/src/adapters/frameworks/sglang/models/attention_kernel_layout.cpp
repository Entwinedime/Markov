#include "markov/adapters/frameworks/sglang/models/attention_kernel_layout.hpp"
#include "markov/production/block_binding.hpp"

#include "markov/contracts/source_observations.hpp"
#include "markov/graph/kernel_shapes.hpp"
#include "markov/graph/source_graph.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <string_view>
#include <utility>
#include <vector>

namespace markov {

AttentionKernelLayout::AttentionKernelLayout(AttentionOperation operation, std::int64_t query_heads, std::int64_t kv_heads, std::int64_t head_dim,
                                             std::int64_t page_size_tokens, std::int64_t cache_pages)
    : operation_(operation),
      query_heads_(query_heads),
      kv_heads_(kv_heads),
      head_dim_(head_dim),
      page_size_tokens_(page_size_tokens),
      cache_pages_(cache_pages) {}

std::optional<AttentionKernelLayout> AttentionKernelLayout::bind(const SourceNativeRegion & region, const SourceKernel & kernel, StepKind step,
                                                                 std::span<const SourceRequest> requests, std::uint32_t page_size_tokens) {
    if (region.semantic != "attention_core") return std::nullopt;
    AttentionOperation operation;
    if (kernel.operation == "ReshapeAndCacheNdKernel") operation = AttentionOperation::CacheWrite;
    else if (kernel.operation == "UnpadFlashAttentionBF16PrefixCacheNdKernel" && step == StepKind::Prefill) operation = AttentionOperation::Prefill;
    else if (kernel.operation == "PagedAttentionMaskNdKernel" && step == StepKind::Decode) operation = AttentionOperation::Decode;
    else return std::nullopt;

    const auto query_heads = region.parameters.at("query_heads");
    const auto kv_heads = region.parameters.at("kv_heads");
    const auto head_dim = region.parameters.at("head_dim");
    std::int64_t tokens = 0;
    std::int64_t max_sequence = 0;
    for (const auto & request : requests) {
        tokens += request.input_tokens;
        max_sequence = std::max(max_sequence, std::int64_t(request.input_tokens) + request.context_tokens);
        if (step == StepKind::Decode && request.input_tokens != 1) return std::nullopt;
    }

    // Qwen3 passes flat Q/K/V to RadixAttention; the backend views them by
    // local heads. An absent K/V or a different layout is a different path.
    for (const auto name : { "q", "k", "v" }) {
        const auto & layout = region.inputs.at(name);
        const auto heads = std::string_view(name) == "q" ? query_heads : kv_heads;
        if (!layout || layout->shape != std::vector<std::int64_t>{ tokens, heads * head_dim }) return std::nullopt;
    }

    const KernelShapes inputs(kernel.input_shapes);
    const KernelShapes outputs(kernel.output_shapes);
    const std::size_t cache_slot = operation == AttentionOperation::CacheWrite ? 2 : 1;
    if (inputs.tensors.size() <= cache_slot) return std::nullopt;
    const auto & cache = inputs.tensors[cache_slot];
    if (cache.size() != 4 || cache[1] != page_size_tokens || cache[2] != kv_heads || cache[3] != head_dim) return std::nullopt;

    const auto batch = static_cast<std::int64_t>(requests.size());
    const std::vector<std::int64_t> blocks{ batch, (max_sequence + page_size_tokens - 1) / page_size_tokens };
    std::vector<std::vector<std::int64_t>> expected_inputs;
    std::vector<std::vector<std::int64_t>> expected_outputs;
    if (operation == AttentionOperation::CacheWrite) {
        expected_inputs = {
            { tokens, kv_heads, head_dim },
            { tokens, kv_heads, head_dim },
            cache,
            cache,
            { tokens }
        };
        expected_outputs = { cache, cache };
    }
    else if (operation == AttentionOperation::Prefill) {
        if (inputs.tensors.size() != 6 || inputs.tensors[4].size() != 2) return std::nullopt;
        // The backend's prebuilt causal mask is fixed. Empty optional slots
        // remain empty; an alibi/FIA/masked variant is not silently adapted.
        expected_inputs = {
            { tokens, query_heads * head_dim },
            cache, cache, blocks, inputs.tensors[4], {}
        };
        expected_outputs = {
            { tokens, query_heads * head_dim }
        };
    }
    else {
        expected_inputs.resize(12);
        expected_inputs[0] = { tokens, query_heads, head_dim };
        expected_inputs[1] = cache;
        expected_inputs[2] = cache;
        expected_inputs[3] = blocks;
        expected_outputs = {
            { tokens, query_heads, head_dim }
        };
    }
    if (inputs.tensors != expected_inputs || outputs.tensors != expected_outputs) return std::nullopt;

    return AttentionKernelLayout(operation, query_heads, kv_heads, head_dim, page_size_tokens, cache[0]);
}

AttentionParameterRule AttentionKernelLayout::compile() const {
    std::vector<TensorParameterAxis> inputs{
        { 0, 1 }
    };
    std::vector<TensorParameterAxis> outputs;
    if (operation_ == AttentionOperation::CacheWrite) {
        inputs.push_back({ 1, 1 });
        inputs.push_back({ 4, 1 });
    }
    else {
        outputs.push_back({ 0, 1 });
        inputs.push_back({ 3, 1, TensorAxisExtent::Requests, 0 });
        inputs.push_back({ 3, 1, TensorAxisExtent::PageColumns, 1 });
    }

    const AttentionWorkload workload{ operation_, {}, query_heads_, kv_heads_, head_dim_, page_size_tokens_, cache_pages_ };
    return AttentionParameterRule(TensorParameterLayout(std::move(inputs), std::move(outputs), page_size_tokens_), workload);
}

} // namespace markov
