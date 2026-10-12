#include "markov/templates/request_pool_parameters.hpp"

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

    // Only indexed accesses consume the current batch's reserved slots. A
    // full-table view changes shape without inventing request membership.
    if (indexed_slots_) {
        target.request_pool_slots.emplace();
        for (const auto & request : input.requests) target.request_pool_slots->push_back(request.pool_slot);
    }
    return target;
}

} // namespace markov
