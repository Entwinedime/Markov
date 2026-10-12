#include "markov/templates/kv_parameters.hpp"

#include "markov/graph/kernel_shapes.hpp"

#include <array>

namespace markov {

std::expected<SourceKernel, std::string> KvParameterLayout::evaluate(const SourceKernel & source, const BatchInput & input,
                                                                     std::optional<std::size_t> request_slot) const {
    if ((kind_ == KvParameterKind::Extend && input.phase != BatchPhase::Prefill) || (kind_ == KvParameterKind::Decode && input.phase != BatchPhase::Decode)
        || input.phase == BatchPhase::Idle)
        return std::unexpected("kv_parameter_phase_not_supported");

    // A moved request region uses the recipient slot, including the releases
    // preceding it. Batch-wide allocation/group release retains its own scope.
    auto release_slot = domain_.release_request_slot;
    if (release_slot && request_slot) release_slot = request_slot;
    const KvBatchWork work(input, page_size_, release_slot);
    if (kind_ == KvParameterKind::Decode && !work.single_token_requests()) return std::unexpected("kv_parameter_requires_one_token_per_request");

    if (kind_ == KvParameterKind::Release) {
        if (work.released_lengths().empty()) return std::unexpected("kv_release_requires_finished_member");
        if (work.free_pages_before_release() < 0 || (work.free_pages_before_release() == 0) != domain_.release_free_empty.value())
            return std::unexpected("kv_release_free_list_branch_not_supported");
    }
    else {
        if (domain_.maximum_new_pages && work.new_pages() > *domain_.maximum_new_pages) return std::unexpected("kv_allocation_page_limit_exceeded");
        if (static_cast<std::uint64_t>(work.new_pages()) > input.free_kv_pages) return std::unexpected("kv_allocation_capacity_exceeded");
        if (domain_.allocates_pages && (work.new_pages() != 0) != *domain_.allocates_pages)
            return std::unexpected("kv_allocation_branch_rewrite_not_implemented");
    }

    const std::array extents{ static_cast<std::int64_t>(input.requests.size()),
                              work.input_tokens(),
                              static_cast<std::int64_t>(input.free_kv_pages),
                              work.released_tokens(),
                              work.released_pages(),
                              work.free_pages_before_release(),
                              work.free_pages_before_release() + work.released_pages() };
    KernelShapes inputs(source.input_shapes);
    KernelShapes outputs(source.output_shapes);
    for (const auto & axis : axes_) {
        auto & operands = axis.output ? outputs.tensors : inputs.tensors;
        operands[axis.slot][0] = extents[static_cast<std::size_t>(axis.extent)];
    }

    SourceKernel target = source;
    if (release_member_inputs_) {
        // Rebuild the qualified homogeneous operand list from target members;
        // individual sequence lengths need not be equal.
        const auto dtype = source.input_dtypes.substr(0, source.input_dtypes.find(';'));
        const auto format = source.input_formats.substr(0, source.input_formats.find(';'));
        inputs.tensors.clear();
        target.input_dtypes.clear();
        target.input_formats.clear();
        for (const auto length : work.released_lengths()) {
            if (!inputs.tensors.empty()) {
                target.input_dtypes += ';';
                target.input_formats += ';';
            }
            inputs.tensors.push_back({ length });
            target.input_dtypes += dtype;
            target.input_formats += format;
        }
    }

    target.input_shapes = inputs.encode();
    target.output_shapes = outputs.encode();
    if (output_ == KvParameterOutput::NewPages) target.kv_new_pages = work.new_pages();
    if (output_ == KvParameterOutput::Allocation) target.kv_allocation = work.allocation();
    if (output_ == KvParameterOutput::Release) target.kv_release = work.release();
    return target;
}

} // namespace markov
