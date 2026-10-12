#include "markov/adapters/frameworks/sglang/paged_kv_layout.hpp"

#include "markov/contracts/source_observations.hpp"
#include "markov/graph/kernel_shapes.hpp"
#include "markov/graph/source_graph.hpp"
#include "markov/templates/templates.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace markov {

std::expected<std::optional<KvParameterLayout>, std::string> PagedKvLayout::bind(const TemplateDefinition & source, const TemplateNode & node,
                                                                                 const std::optional<std::vector<SourceRequest>> & finished,
                                                                                 std::uint32_t page_size_tokens) {
    for (const auto & region : source.regions) {
        if (!region.module || source.observation(node).source_scope != region.source_call) continue;
        const auto & module = *region.module;

        if (module.semantic == "kv_allocation") {
            if (module.method == "alloc_extend") return extend(*source.observation(node).kernel, module, source.source_requests, page_size_tokens);
            if (module.method == "alloc_decode") return decode(*source.observation(node).kernel, module, page_size_tokens);
        }
        if (source.step != StepKind::Publish) break;
        if (source.phase == BatchPhase::Prefill) {
            if (module.semantic != "kv_release" || module.method != "free" || module.parameters.at("is_not_in_free_group") != 1) break;
            if (!region.request_slot || source.publication_finishes[*region.request_slot] != std::optional(true))
                return std::unexpected("kv_direct_release_request_not_qualified");

            const auto request = std::span(source.source_requests).subspan(*region.request_slot, 1);
            return release(*source.observation(node).kernel, module, request, page_size_tokens, region.request_slot);
        }
        if (source.phase != BatchPhase::Decode) break;
        const bool group_end = module.semantic == "kv_release_group" && module.method == "free_group_end";
        const bool grouped_free = module.semantic == "kv_release" && module.method == "free" && module.parameters.at("is_not_in_free_group") == 1;
        if (!group_end && !grouped_free) break;

        // Device release belongs to free nested in group end. Per-request
        // frees only enqueue indices and must not become duplicate releases.
        if (grouped_free) {
            const auto parent = std::find_if(source.regions.begin(), source.regions.end(), [&](const TemplateRegion & candidate) {
                return candidate.source_call == region.parent_call;
            });
            if (parent == source.regions.end() || !parent->module || parent->module->method != "free_group_end")
                return std::unexpected("npu_release_group_parent_missing");
        }
        if (!finished) return std::unexpected("kv_release_source_publication_missing");
        return release(*source.observation(node).kernel, module, *finished, page_size_tokens);
    }

    if (source.observation(node).kernel->name == "alloc_extend_kernel" || source.observation(node).kernel->name == "alloc_decode_kernel")
        return std::unexpected("paged_kv_allocator_scope_unresolved");
    return std::optional<KvParameterLayout>{};
}

std::expected<KvParameterLayout, std::string> PagedKvLayout::release(const SourceKernel & source, const SourceNativeRegion & region,
                                                                     std::span<const SourceRequest> source_finished, std::uint32_t page_size_tokens,
                                                                     std::optional<std::size_t> request_slot) {
    const bool group = region.method == "free_group_end";
    if (region.module_type != "sglang.srt.hardware_backend.npu.allocator_npu.NPUPagedTokenToKVPoolAllocator" || (!group && region.method != "free")
        || region.parameters.at("page_size") != page_size_tokens || region.parameters.at("need_sort") != 0
        || region.parameters.at("is_not_in_free_group") != (group ? 0 : 1))
        return std::unexpected("npu_grouped_release_state_not_supported");

    std::vector<std::vector<std::int64_t>> source_lengths;
    std::int64_t source_tokens = 0;
    std::int64_t source_pages = 0;
    for (const auto & request : source_finished) {
        const auto length = static_cast<std::int64_t>(request.context_tokens) + request.input_tokens;
        source_lengths.push_back({ length });
        source_tokens += length;
        source_pages += (length + page_size_tokens - 1) / page_size_tokens;
    }

    if (source_lengths.empty()) return std::unexpected("npu_grouped_release_membership_rewrite_not_implemented");
    const auto source_free = region.inputs.at("free_pages")->shape.at(0);

    KernelShapes inputs(source.input_shapes);
    KernelShapes outputs(source.output_shapes);
    std::vector<std::vector<std::int64_t>> expected;
    std::int64_t expected_output;
    bool final_return = false;
    if (group) {
        if (source_lengths.size() < 2 || source_lengths.size() > release_concat_limit || source.name != "aclnnCat_ConcatD_ConcatD"
            || source.operation != "ConcatD" || source.output_dtypes != "INT32")
            return std::unexpected("npu_grouped_release_operand_unmapped");
        std::string dtypes = "INT32";
        for (std::size_t index = 1; index < source_lengths.size(); ++index) dtypes += ";INT32";
        std::string formats = "ND";
        for (std::size_t index = 1; index < source_lengths.size(); ++index) formats += ";ND";
        if (source.input_dtypes != dtypes || source.input_formats != formats || source.output_formats != "ND")
            return std::unexpected("npu_grouped_release_operand_unmapped");
        expected = source_lengths;
        expected_output = source_tokens;
    }
    else {
        const auto & free_index = region.inputs.at("free_index");
        if (!free_index || free_index->shape != std::vector<std::int64_t>{ source_tokens })
            return std::unexpected("npu_grouped_release_source_lengths_disagree");
        if (source.name == "aclnnCat_CastAiCore_Cast" && source.operation == "Cast" && source.input_dtypes == "INT32" && source.output_dtypes == "INT64") {
            expected = { { source_pages } };
            expected_output = source_pages;
        }
        else if (source.name == "aclnnCat_ConcatD_ConcatD" && source.operation == "ConcatD" && source.input_dtypes == "INT64;INT64"
                 && source.output_dtypes == "INT64") {
            expected = { { source_pages }, { source_free } };
            expected_output = source_pages + source_free;
            final_return = true;
        }
        else return std::unexpected("npu_grouped_release_operand_unmapped");
    }
    if (inputs.tensors != expected || outputs.tensors != std::vector<std::vector<std::int64_t>>{ { expected_output } })
        return std::unexpected("npu_grouped_release_operand_unmapped");

    std::vector<KvParameterAxis> axes;
    if (group) { axes.push_back({ true, 0, KvParameterExtent::ReleasedTokens }); }
    else {
        axes.push_back({ false, 0, KvParameterExtent::ReleasedPages });
        if (final_return) axes.push_back({ false, 1, KvParameterExtent::ReleaseFreePages });
        axes.push_back({ true, 0, final_return ? KvParameterExtent::ReleaseTotalPages : KvParameterExtent::ReleasedPages });
    }

    KvParameterDomain domain;
    domain.release_free_empty = source_free == 0;
    domain.release_request_slot = request_slot;
    return KvParameterLayout(KvParameterKind::Release,
                             page_size_tokens,
                             domain,
                             std::move(axes),
                             final_return ? KvParameterOutput::Release : KvParameterOutput::None,
                             group);
}

std::expected<KvParameterLayout, std::string> PagedKvLayout::decode(const SourceKernel & source, const SourceNativeRegion & region,
                                                                    std::uint32_t page_size_tokens) {
    // This native implementation has different node sets for zero/new pages.
    // Shape substitution is only defined within the observed branch.
    if (region.module_type != "sglang.srt.hardware_backend.npu.allocator_npu.NPUPagedTokenToKVPoolAllocator" || region.method != "alloc_decode"
        || region.parameters.at("page_size") != page_size_tokens || region.parameters.at("need_sort") != 0 || region.parameters.at("is_not_in_free_group") != 1)
        return std::unexpected("npu_decode_allocator_state_not_supported");

    const auto & lengths = region.cpu_values.at("seq_lens_cpu");
    const auto source_batch = static_cast<std::int64_t>(lengths.size());
    const auto source_free = region.inputs.at("free_pages")->shape.at(0);
    std::uint64_t source_new_pages = 0;
    for (const auto length : lengths) source_new_pages += length % page_size_tokens == 1;

    if (source_new_pages > static_cast<std::uint64_t>(source_free)) return std::unexpected("npu_decode_allocator_merge_or_exhaustion_not_supported");

    KernelShapes inputs(source.input_shapes);
    KernelShapes outputs(source.output_shapes);
    std::vector<std::vector<std::int64_t>> expected;
    std::vector<std::size_t> request_slots;
    bool free_page_lookup = false;
    bool result_node = false;
    const bool int64_result = source.output_dtypes == "INT64";
    const bool int32_result = source.output_dtypes == "INT32";

    // Operand roles follow the allocator body, not numerical equality with B.
    // In particular, cumsum's axis and Index's descriptor slots stay constant at 1.
    if (source.operation == "BroadcastTo" && source.input_dtypes == "INT64;INT64" && int64_result) { expected = { {}, { 1 } }; }
    else if ((source.operation == "FloorMod" || source.operation == "Sub" || source.operation == "Add" || source.operation == "Mul")
             && source.input_dtypes == "INT64;INT64" && int64_result
             && inputs.tensors == std::vector<std::vector<std::int64_t>>{ { source_batch }, { source_batch } }) {
        expected = { { source_batch }, { source_batch } };
        request_slots = { 0, 1 };
    }
    else if (source.operation == "Equal" && source.input_dtypes == "INT64;INT64" && source.output_dtypes == "BOOL") {
        expected = { { source_batch }, {} };
        request_slots = { 0 };
    }
    else if (source.operation == "Cast"
             && ((source.input_dtypes == "BOOL" && int32_result) || (source.input_dtypes == "INT32" && int64_result)
                 || (source.input_dtypes == "INT64" && int32_result))) {
        expected = { { source_batch } };
        request_slots = { 0 };
        result_node = source_new_pages != 0 && source.input_dtypes == "INT64" && int32_result;
    }
    else if (source.operation == "Cumsum" && source.input_dtypes == "INT64;INT64" && int64_result) {
        expected = { { source_batch }, { 1 } };
        request_slots = { 0 };
    }
    else if (source.operation == "Add" && source.input_dtypes == "INT32;INT32" && int32_result) {
        expected = { { source_batch }, {} };
        request_slots = { 0 };
        result_node = source_new_pages == 0;
    }
    else if (source.operation == "Sub" && source.input_dtypes == "INT32;INT32" && int32_result) {
        expected = { {}, { source_batch } };
        request_slots = { 1 };
    }
    else if (source.operation == "Mul" && source.input_dtypes == "INT32;INT32" && int32_result) {
        expected = { { source_batch }, { source_batch } };
        request_slots = { 0, 1 };
    }
    else if (source.operation == "Mul" && source.input_dtypes == "INT64;INT64" && int64_result) {
        expected = { { source_batch }, {} };
        request_slots = { 0 };
    }
    else if (source.operation == "Index" && source.input_dtypes == "INT64;INT64;INT64;INT64" && int64_result) {
        expected = { { source_free }, { 1 }, { 1 }, { source_batch } };
        request_slots = { 3 };
        free_page_lookup = true;
    }
    else return std::unexpected("npu_decode_allocator_operand_unmapped");

    if (inputs.tensors != expected || outputs.tensors != std::vector<std::vector<std::int64_t>>{ { source_batch } })
        return std::unexpected("npu_decode_allocator_operand_unmapped");

    std::vector<KvParameterAxis> axes;
    for (const auto slot : request_slots) axes.push_back({ false, slot, KvParameterExtent::Requests });
    if (free_page_lookup) axes.push_back({ false, 0, KvParameterExtent::FreePages });
    axes.push_back({ true, 0, KvParameterExtent::Requests });

    KvParameterDomain domain;
    domain.allocates_pages = source_new_pages != 0;
    return KvParameterLayout(KvParameterKind::Decode,
                             page_size_tokens,
                             domain,
                             std::move(axes),
                             result_node ? KvParameterOutput::Allocation : KvParameterOutput::None);
}

std::expected<KvParameterLayout, std::string> PagedKvLayout::extend(const SourceKernel & source, const SourceNativeRegion & region,
                                                                    std::span<const SourceRequest> source_requests, std::uint32_t page_size_tokens) {
    if (region.module_type != "sglang.srt.hardware_backend.npu.allocator_npu.NPUPagedTokenToKVPoolAllocator" || region.method != "alloc_extend"
        || region.parameters.at("page_size") != page_size_tokens || region.parameters.at("need_sort") != 0 || region.parameters.at("is_not_in_free_group") != 1)
        return std::unexpected("npu_extend_allocator_state_not_supported");

    const auto & prefixes = region.cpu_values.at("prefix_lens_cpu");
    const auto & lengths = region.cpu_values.at("seq_lens_cpu");
    if (prefixes.size() != source_requests.size() || lengths.size() != source_requests.size())
        return std::unexpected("npu_extend_allocator_source_lengths_disagree");
    std::int64_t source_tokens = 0;
    std::int64_t source_pages = 0;
    for (std::size_t index = 0; index < source_requests.size(); ++index) {
        const auto & request = source_requests[index];
        if (prefixes[index] != request.context_tokens || lengths[index] != prefixes[index] + request.input_tokens)
            return std::unexpected("npu_extend_allocator_source_lengths_disagree");
        source_tokens += request.input_tokens;
        source_pages += (lengths[index] + page_size_tokens - 1) / page_size_tokens - (prefixes[index] + page_size_tokens - 1) / page_size_tokens;
    }
    const auto & returned = region.outputs.at("out_cache_loc");
    if (region.call_parameters.at("extend_num_tokens") != source_tokens || !returned || returned->dtype != "torch.int32"
        || returned->shape != std::vector<std::int64_t>{ source_tokens })
        return std::unexpected("npu_extend_allocator_return_unmapped");

    const auto source_free = region.inputs.at("free_pages")->shape.at(0);
    // Official NPU alloc_extend switches to alloc_extend_naive at 200 new pages.
    if (source_pages >= 200) return std::unexpected("npu_extend_allocator_naive_branch_not_supported");
    if (source_pages > source_free) return std::unexpected("npu_extend_allocator_exhaustion_not_supported");

    const auto source_batch = static_cast<std::int64_t>(source_requests.size());
    KernelShapes inputs(source.input_shapes);
    KernelShapes outputs(source.output_shapes);
    std::vector<std::vector<std::int64_t>> expected;
    std::vector<KvParameterAxis> axes;
    std::vector<std::vector<std::int64_t>> expected_output{ { source_batch } };
    KvParameterExtent output_extent = KvParameterExtent::Requests;
    bool page_sum = false;
    bool final_return = false;
    if (((source.name == "aclnnAdds_AddAiCore_Add" && source.operation == "Add")
         || (source.name == "aclnnFloorDivides_FloorDivAiCore_FloorDiv" && source.operation == "FloorDiv"))
        && source.input_dtypes == "INT64;INT64" && source.output_dtypes == "INT64") {
        expected = { { source_batch }, {} };
        axes = {
            { false, 0, KvParameterExtent::Requests }
        };
    }
    else if (source.name == "aclnnSub_SubAiCore_Sub" && source.operation == "Sub" && source.input_dtypes == "INT64;INT64" && source.output_dtypes == "INT64") {
        expected = { { source_batch }, { source_batch } };
        axes = {
            { false, 0, KvParameterExtent::Requests },
            { false, 1, KvParameterExtent::Requests }
        };
    }
    else if (source.name == "aclnnReduceSum_ReduceSumOpAiCore_ReduceSum" && source.operation == "ReduceSum" && source.input_dtypes == "INT64;INT64"
             && source.output_dtypes == "INT64") {
        expected = { { source_batch }, { 1 } };
        axes = {
            { false, 0, KvParameterExtent::Requests }
        };
        expected_output = { {} };
        page_sum = true;
    }
    else if (source.name == "alloc_extend_kernel" && source.operation == "alloc_extend_kernel" && source.input_dtypes == "INT64;INT64;INT64;INT64"
             && source.output_dtypes == "INT64") {
        expected = { { source_batch }, { source_batch }, { source_batch }, { source_free } };
        axes = {
            { false, 0,  KvParameterExtent::Requests },
            { false, 1,  KvParameterExtent::Requests },
            { false, 2,  KvParameterExtent::Requests },
            { false, 3, KvParameterExtent::FreePages }
        };
        expected_output = { { source_tokens } };
        output_extent = KvParameterExtent::Tokens;
    }
    else if (source.name == "aclnnInplaceCopy_CastAiCore_Cast" && source.operation == "Cast" && source.input_dtypes == "INT64"
             && source.output_dtypes == "INT32") {
        expected = expected_output = { { source_tokens } };
        axes = {
            { false, 0, KvParameterExtent::Tokens }
        };
        output_extent = KvParameterExtent::Tokens;
        final_return = true;
    }
    else return std::unexpected("npu_extend_allocator_operand_unmapped");
    if (inputs.tensors != expected || outputs.tensors != expected_output) return std::unexpected("npu_extend_allocator_operand_unmapped");

    if (!page_sum) axes.push_back({ true, 0, output_extent });
    KvParameterDomain domain;
    domain.maximum_new_pages = 199; // Native alloc_extend_naive starts at 200.
    const auto output = page_sum ? KvParameterOutput::NewPages : final_return ? KvParameterOutput::Allocation : KvParameterOutput::None;
    return KvParameterLayout(KvParameterKind::Extend, page_size_tokens, domain, std::move(axes), output);
}

} // namespace markov
