#include "markov/templates/tensor_parameters.hpp"

#include "markov/graph/kernel_shapes.hpp"

#include <algorithm>
#include <array>

namespace markov {

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
