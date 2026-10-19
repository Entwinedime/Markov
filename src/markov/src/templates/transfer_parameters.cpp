#include "markov/templates/transfer_parameters.hpp"


namespace markov {

std::expected<SourceTransfer, std::string> TransferParameterLayout::evaluate(const SourceTransfer & source, const BatchInput & input) const {
    if (extent == TransferExtent::TransitionRetainedRequests || extent == TransferExtent::RunningRetainedRequests) {
        // Filtering precedes batch selection: its retained membership is not
        // necessarily the membership of the batch selected for execution.
        const auto prefix_count = input.transition.pending_prefill_requests ? 1U : 0U;
        const bool transition = extent == TransferExtent::TransitionRetainedRequests;
        if ((transition && (prefix_count == 0 || input.filters.empty())) || (!transition && input.filters.size() != prefix_count + 1))
            return std::unexpected("transfer_filter_role_not_present");

        const auto & filter = input.filters[transition ? 0 : prefix_count];
        if (filter.retained_indices.empty() || filter.retained_indices.size() == filter.input_count)
            return std::unexpected("transfer_filter_branch_rewrite_not_implemented");

        auto target = source;
        target.bytes = filter.retained_indices.size() * element_bytes;
        return target;
    }

    // Element and token extents describe target work. Only fixed branches
    // depend on the source batch count; releases use the finished membership.
    if (extent == TransferExtent::RequestElements) {
        auto target = source;
        target.bytes = input.requests.size() * element_bytes;
        return target;
    }
    if (extent == TransferExtent::InputTokens) {
        if (input.phase != BatchPhase::Prefill) return std::unexpected("input_transfer_phase_not_qualified");
        auto target = source;
        target.bytes = 0;
        for (const auto & request : input.requests) target.bytes += static_cast<std::uint64_t>(request.input_tokens) * element_bytes;
        return target;
    }
    if (extent == TransferExtent::FixedAtRequestCount) {
        if (input.requests.size() != request_count) return std::unexpected("transfer_request_count_not_qualified");
        return source;
    }

    if (input.phase != BatchPhase::Decode) return std::unexpected("release_transfer_phase_not_qualified");

    std::size_t finished = 0;
    std::uint64_t tokens = 0;
    for (const auto & request : input.requests) {
        if (!request.finishes) continue;
        ++finished;
        tokens += static_cast<std::uint64_t>(request.context_tokens) + request.input_tokens;
    }
    if (finished != finished_count) return std::unexpected("release_transfer_membership_not_qualified");

    // These are token-index elements, not the number of physical KV pages.
    auto target = source;
    target.bytes = tokens * element_bytes;
    return target;
}

} // namespace markov
