#include "markov/adapters/frameworks/sglang/batch_layout.hpp"

#include "markov/graph/kernel_shapes.hpp"

#include <cstdint>
#include <utility>
#include <vector>

namespace markov {

std::expected<std::optional<BatchParameterLayout>, std::string> BatchLayout::bind_decode_lengths(const SourceKernel & source, std::size_t requests) const {
    const bool add = source.name == "aclnnAdds_AddAiCore_Add" && source.operation == "Add";
    const bool subtract = source.name == "aclnnSubs_SubAiCore_Sub" && source.operation == "Sub";
    if (!add && !subtract) return std::optional<BatchParameterLayout>{};

    // In ordinary paged decode, direct prepare arithmetic computes last/next
    // lengths and updates seq_lens/orig_seq_lens, all with one row per request.
    // The caller excludes nested allocator scopes; no tensor identity is
    // inferred from the ordering of these otherwise identical operations.
    const auto rows = static_cast<std::int64_t>(requests);
    const KernelShapes inputs(source.input_shapes);
    const KernelShapes outputs(source.output_shapes);
    if ((source.output_dtypes != "INT64" && source.output_dtypes != "INT32") || source.input_dtypes != source.output_dtypes + ";" + source.output_dtypes
        || inputs.tensors != std::vector<std::vector<std::int64_t>>{ { rows }, {} } || outputs.tensors != std::vector<std::vector<std::int64_t>>{ { rows } })
        return std::unexpected("decode_length_operand_unmapped");

    return BatchParameterLayout(std::nullopt,
                                {
                                    { false, 0, BatchRowExtent::Requests },
                                    {  true, 0, BatchRowExtent::Requests }
    });
}

std::expected<std::optional<BatchParameterLayout>, std::string> BatchLayout::bind(const SourceKernel & source, std::span<const BatchFilter> filters,
                                                                                  const std::optional<BatchMerge> & observed_merge,
                                                                                  std::optional<NativeFilterSelection> selection) const {
    auto filtered = filter(source, filters, selection);
    if (selection || !filtered || *filtered) return filtered;
    return bind_merge(source, observed_merge);
}

std::expected<std::optional<BatchParameterLayout>, std::string> BatchLayout::filter(const SourceKernel & source, std::span<const BatchFilter> source_filters,
                                                                                    std::optional<NativeFilterSelection> selection) const {
    if (source.name != "aclnnIndex_IndexAiCore_Index" || source.operation != "Index") return std::optional<BatchParameterLayout>{};

    // Native parent scopes establish which control action owns this filter.
    // An unscoped Index cannot inherit identity from the number of calls.
    if (!selection) return std::unexpected("batch_filter_scope_unqualified");

    const auto & observed = source_filters[selection->index];
    if (observed.retained_indices.empty() || observed.retained_indices.size() == observed.input_count)
        return std::unexpected("batch_filter_scope_branch_mismatch");

    KernelShapes inputs(source.input_shapes);
    KernelShapes outputs(source.output_shapes);
    if (inputs.tensors.size() != 4 || inputs.tensors[0].empty() || inputs.tensors[0].size() > 2
        || (source.output_dtypes != "INT64" && source.output_dtypes != "INT32" && source.output_dtypes != "FLOAT")
        || source.input_dtypes != source.output_dtypes + ";INT64;INT64;INT64")
        return std::unexpected("batch_filter_operand_unmapped");

    const auto rows = static_cast<std::int64_t>(observed.input_count);
    const auto kept = static_cast<std::int64_t>(observed.retained_indices.size());
    const auto dimensions = static_cast<std::int64_t>(inputs.tensors[0].size());
    auto expected_input = inputs.tensors[0];
    expected_input[0] = rows;
    auto expected_output = expected_input;
    expected_output[0] = kept;
    const std::vector<std::vector<std::int64_t>> expected{ expected_input, { 1 }, { dimensions }, { kept } };
    if (inputs.tensors != expected || outputs.tensors != std::vector<std::vector<std::int64_t>>{ expected_output })
        return std::unexpected("batch_filter_operand_unmapped");

    return BatchParameterLayout(selection->role,
                                {
                                    { false, 0, BatchRowExtent::FilterInput },
                                    { false, 3,  BatchRowExtent::FilterKept },
                                    {  true, 0,  BatchRowExtent::FilterKept }
    });
}

std::expected<std::optional<BatchParameterLayout>, std::string> BatchLayout::bind_merge(const SourceKernel & source,
                                                                                        const std::optional<BatchMerge> & observed) const {
    if (source.name != "aclnnCat_ConcatD_ConcatD" || source.operation != "ConcatD") return std::optional<BatchParameterLayout>{};
    if (!observed) return std::unexpected("batch_merge_source_membership_unresolved");

    KernelShapes inputs(source.input_shapes);
    KernelShapes outputs(source.output_shapes);
    if (inputs.tensors.size() != 2 || inputs.tensors[0].empty() || inputs.tensors[0].size() > 2
        || (source.output_dtypes != "INT64" && source.output_dtypes != "INT32" && source.output_dtypes != "FLOAT")
        || source.input_dtypes != source.output_dtypes + ";" + source.output_dtypes)
        return std::unexpected("batch_merge_operand_unmapped");

    auto left = inputs.tensors[0];
    left[0] = static_cast<std::int64_t>(observed->left_count);
    auto right = left;
    right[0] = static_cast<std::int64_t>(observed->right_count);
    auto joined = left;
    joined[0] += right[0];
    if (inputs.tensors != std::vector<std::vector<std::int64_t>>{ left, right } || outputs.tensors != std::vector<std::vector<std::int64_t>>{ joined })
        return std::unexpected("batch_merge_operand_unmapped");

    // Preserve native running-then-prefill order; target membership supplies
    // row counts without reinterpreting the native operation in the core.
    return BatchParameterLayout(std::nullopt,
                                {
                                    { false, 0,  BatchRowExtent::MergeLeft },
                                    { false, 1, BatchRowExtent::MergeRight },
                                    {  true, 0, BatchRowExtent::MergeTotal }
    });
}

} // namespace markov
