#include "markov/templates/batch_parameters.hpp"

#include "markov/graph/kernel_shapes.hpp"

#include <algorithm>
#include <array>

namespace markov {

bool BatchOperationVariant::accepts(const BatchInput & input) const {
    const auto requests = input.requests.size();
    if (requests < minimum_requests || requests > maximum_requests) return false;
    if (!page_table) return true;

    std::uint64_t sequence = 0;
    for (const auto & request : input.requests) sequence = std::max(sequence, static_cast<std::uint64_t>(request.context_tokens) + request.input_tokens);

    // The selected page columns form either a contiguous vector, a column
    // spanning several rows, or a matrix with a stride between page entries.
    auto materialization = PageTableMaterialization::Contiguous;
    if (page_size_tokens > 1 && sequence > page_size_tokens) materialization = PageTableMaterialization::Strided;
    else if (page_size_tokens > 1 && requests > 1 && sequence > 1) materialization = PageTableMaterialization::SingleColumn;
    return *page_table == materialization;
}

std::expected<SourceKernel, std::string> BatchParameterLayout::evaluate(const SourceKernel & source, const BatchInput & input,
                                                                        std::optional<std::size_t> request_slot) const {
    std::array<std::int64_t, 7> extents{};
    extents[static_cast<std::size_t>(BatchRowExtent::Requests)] = static_cast<std::int64_t>(input.requests.size());
    SourceKernel target = source;
    if (filter_) {
        const auto prefix_count = input.transition.pending_prefill_requests ? 1U : 0U;
        const bool transition = *filter_ == BatchFilterBinding::Transition;
        if ((transition && prefix_count == 0) || (!transition && input.filters.size() != prefix_count + 1))
            return std::unexpected("batch_filter_role_not_present");

        const auto & requested = input.filters[transition ? 0 : prefix_count];
        if (requested.retained_indices.empty() || requested.retained_indices.size() == requested.input_count)
            return std::unexpected("batch_filter_branch_rewrite_not_implemented");

        extents[static_cast<std::size_t>(BatchRowExtent::FilterInput)] = static_cast<std::int64_t>(requested.input_count);
        extents[static_cast<std::size_t>(BatchRowExtent::FilterKept)] = static_cast<std::int64_t>(requested.retained_indices.size());
        target.batch_filter = requested;
    }
    else if (request_) {
        // An explicit slot is supplied only by qualified member relocation.
        // Unmoved observations still require their original batch membership.
        if (!request_slot && input.requests.size() != request_->request_count) return std::unexpected("request_sequence_membership_rewrite_not_implemented");
        const auto & request = input.requests[request_slot.value_or(request_->slot)];
        extents[static_cast<std::size_t>(BatchRowExtent::RequestSequenceTokens)] = static_cast<std::int64_t>(request.context_tokens) + request.input_tokens;
    }
    else if (rows_.front().extent != BatchRowExtent::Requests) {
        if (!input.merge) return std::unexpected("batch_merge_branch_rewrite_not_implemented");
        extents[static_cast<std::size_t>(BatchRowExtent::MergeLeft)] = static_cast<std::int64_t>(input.merge->left_count);
        extents[static_cast<std::size_t>(BatchRowExtent::MergeRight)] = static_cast<std::int64_t>(input.merge->right_count);
        extents[static_cast<std::size_t>(BatchRowExtent::MergeTotal)] =
            extents[static_cast<std::size_t>(BatchRowExtent::MergeLeft)] + extents[static_cast<std::size_t>(BatchRowExtent::MergeRight)];
        target.batch_merge = input.merge;
    }

    KernelShapes inputs(source.input_shapes);
    KernelShapes outputs(source.output_shapes);
    for (const auto & row : rows_) {
        auto & operands = row.output ? outputs.tensors : inputs.tensors;
        operands[row.slot][0] = extents[static_cast<std::size_t>(row.extent)];
    }
    if (input_arity_ == BatchInputArity::Requests) {
        // The producer qualifies a homogeneous list. Keep its tensor geometry,
        // dtype and format together when rebuilding one operand per request.
        const auto member_shape = inputs.tensors.front();
        const auto member_dtype = source.input_dtypes.substr(0, source.input_dtypes.find(';'));
        const auto member_format = source.input_formats.substr(0, source.input_formats.find(';'));
        inputs.tensors.assign(input.requests.size(), member_shape);
        target.input_dtypes.clear();
        target.input_formats.clear();

        for (std::size_t index = 0; index < input.requests.size(); ++index) {
            if (index != 0) {
                target.input_dtypes += ';';
                target.input_formats += ';';
            }
            target.input_dtypes += member_dtype;
            target.input_formats += member_format;
        }
    }

    target.input_shapes = inputs.encode();
    target.output_shapes = outputs.encode();
    return target;
}

} // namespace markov
