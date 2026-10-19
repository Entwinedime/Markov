#include "markov/templates/request_pool_parameters.hpp"

#include <utility>
#include <vector>

namespace markov {

std::optional<std::string> RequestPoolCapacityDomain::unsupported(std::uint32_t capacity) const {
    if (capacity == maximum) return std::nullopt;
    if (capacity > maximum) return "request_pool_capacity_increase_not_supported";
    return shrink_gap;
}

std::expected<SourceKernel, std::string> RequestPoolParameterLayout::evaluate(const SourceKernel & source, const BatchInput & input,
                                                                              std::uint32_t capacity) const {
    if (single_request_only_ && input.requests.size() != 1) return std::unexpected("indexed_write_branch_replacement_required");
    SourceKernel target = shapes_.evaluate(source, input, capacity);

    // The producer supplies the access role. Target request state supplies its
    // actual rows and token ranges; a full-table view has no indexed access.
    if (!access_) return target;
    switch (*access_) {
    case RequestPoolOperation::PrefixExtendWrite: {
        std::vector<RequestPoolRange> ranges;
        for (const auto & request : input.requests) ranges.push_back({ request.pool_slot, request.context_tokens, request.input_tokens });
        target.request_pool = RequestPoolWork{ *access_, std::move(ranges) };
        break;
    }
    case RequestPoolOperation::TokenRead:
    case RequestPoolOperation::TokenWrite: {
        std::vector<RequestPoolPoint> points;
        for (const auto & request : input.requests) {
            const auto column = static_cast<std::int64_t>(request.context_tokens) - (*access_ == RequestPoolOperation::TokenRead ? 1 : 0);
            points.push_back({ request.pool_slot, column });
        }
        target.request_pool = RequestPoolWork{ *access_, std::move(points) };
        break;
    }
    case RequestPoolOperation::RowRead:
    case RequestPoolOperation::RelayRead:
    case RequestPoolOperation::RelayWrite: {
        std::vector<std::uint32_t> slots;
        for (const auto & request : input.requests) slots.push_back(request.pool_slot);
        target.request_pool = RequestPoolWork{ *access_, std::move(slots) };
        break;
    }
    }
    return target;
}

} // namespace markov
