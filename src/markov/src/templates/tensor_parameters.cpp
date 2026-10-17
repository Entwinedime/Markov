#include "markov/templates/tensor_parameters.hpp"

#include "markov/graph/kernel_shapes.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>

namespace markov {

nlohmann::json TensorParameterLayout::describe_parameters() const {
    static constexpr std::array names{
        "input_tokens", "requests", "maximum_sequence_tokens", "page_columns", "sequence_storage_elements", "request_pool_rows"
    };
    nlohmann::json axes = nlohmann::json::array();
    for (const auto & axis : inputs_) {
        axes.push_back({
            {    "operand",                                      "input" },
            {       "slot",                                    axis.slot },
            {       "axis",                                    axis.axis },
            { "multiplier",                              axis.multiplier },
            {     "extent", names[static_cast<std::size_t>(axis.extent)] }
        });
    }
    for (const auto & axis : outputs_) {
        axes.push_back({
            {    "operand",                                     "output" },
            {       "slot",                                    axis.slot },
            {       "axis",                                    axis.axis },
            { "multiplier",                              axis.multiplier },
            {     "extent", names[static_cast<std::size_t>(axis.extent)] }
        });
    }

    // Only these axes vary; unlisted dimensions retain the source descriptor.
    // Extent names describe relations, never samples or a target range.
    return {
        {      "tensor_axes",   std::move(axes) },
        { "page_size_tokens", page_size_tokens_ }
    };
}

SourceKernel TensorParameterLayout::evaluate(const SourceKernel & source, const BatchInput & input, std::optional<std::uint32_t> request_capacity) const {
    std::int64_t tokens = 0;
    std::int64_t max_sequence = 0;
    for (const auto & request : input.requests) {
        tokens += request.input_tokens;
        max_sequence = std::max(max_sequence, static_cast<std::int64_t>(request.context_tokens) + request.input_tokens);
    }

    const auto requests = static_cast<std::int64_t>(input.requests.size());
    const std::array extents{ tokens,
                              requests,
                              max_sequence,
                              page_size_tokens_ ? (max_sequence + page_size_tokens_ - 1) / page_size_tokens_ : 0,
                              requests * max_sequence };

    // Only declared parameter axes vary. Weight orientation, dtype, format and kernel
    // identity remain observations, subject to separate topology qualification.
    KernelShapes inputs(source.input_shapes);
    KernelShapes outputs(source.output_shapes);
    for (const auto & axis : inputs_) {
        const auto extent = axis.extent == TensorAxisExtent::RequestPoolRows ? static_cast<std::int64_t>(request_capacity.value()) + 1
                                                                             : extents[static_cast<std::size_t>(axis.extent)];
        inputs.tensors[axis.slot][axis.axis] = extent * axis.multiplier;
    }
    for (const auto & axis : outputs_) {
        const auto extent = axis.extent == TensorAxisExtent::RequestPoolRows ? static_cast<std::int64_t>(request_capacity.value()) + 1
                                                                             : extents[static_cast<std::size_t>(axis.extent)];
        outputs.tensors[axis.slot][axis.axis] = extent * axis.multiplier;
    }

    SourceKernel target = source;
    target.input_shapes = inputs.encode();
    target.output_shapes = outputs.encode();

    return target;
}

} // namespace markov
