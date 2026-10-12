#include "markov/templates/kv_workload.hpp"

namespace markov {

KvBatchWork::KvBatchWork(const BatchInput & input, std::uint32_t page_size_tokens, std::optional<std::size_t> release_request_slot)
    : input_(input),
      page_size_tokens_(page_size_tokens),
      release_request_slot_(release_request_slot) {
    for (std::size_t slot = 0; slot < input.requests.size(); ++slot) {
        const auto & request = input.requests[slot];
        const auto prefix = static_cast<std::int64_t>(request.context_tokens);
        const auto length = prefix + request.input_tokens;
        input_tokens_ += request.input_tokens;
        new_pages_ += (length + page_size_tokens - 1) / page_size_tokens - (prefix + page_size_tokens - 1) / page_size_tokens;
        single_token_requests_ = single_token_requests_ && request.input_tokens == 1;

        if (!request.finishes) continue;
        // Direct request releases run in publication order. Earlier releases
        // enlarge the free list seen by this request, without releasing it twice.
        if (release_request_slot_) {
            if (slot < *release_request_slot_) prior_released_pages_ += static_cast<std::int64_t>(request.kv_pages.size());
            if (slot != *release_request_slot_) continue;
        }
        released_lengths_.push_back(length);
        released_tokens_ += length;
        released_pages_ += static_cast<std::int64_t>(request.kv_pages.size());
    }
}

std::int64_t KvBatchWork::free_pages_before_release() const {
    // BatchInput describes the state before Prepare. Publication follows all
    // reservations in this batch, including requests which have not finished.
    return static_cast<std::int64_t>(input_.free_kv_pages) - new_pages_ + prior_released_pages_;
}

KvAllocationWorkload KvBatchWork::allocation() const {
    KvAllocationWorkload workload{ page_size_tokens_, input_.free_kv_pages, {} };
    for (const auto & request : input_.requests)
        workload.sequences.push_back({ request.input_tokens, request.context_tokens, request.pool_slot, request.kv_pages });
    return workload;
}

KvReleaseWorkload KvBatchWork::release() const {
    // The caller qualifies capacity and release-branch constraints first.
    KvReleaseWorkload workload{ page_size_tokens_, static_cast<std::uint64_t>(free_pages_before_release()), {} };
    for (std::size_t slot = 0; slot < input_.requests.size(); ++slot) {
        const auto & request = input_.requests[slot];
        if (request.finishes && (!release_request_slot_ || slot == *release_request_slot_))
            workload.sequences.push_back({ request.input_tokens, request.context_tokens, request.pool_slot, request.kv_pages });
    }
    return workload;
}

} // namespace markov
