#include "markov/templates/kv_workload.hpp"

namespace markov {

KvBatchWork::KvBatchWork(const BatchInput & input, std::uint32_t page_size_tokens, std::optional<std::size_t> release_request_slot)
    : free_pages_before_release_(static_cast<std::int64_t>(input.free_kv_pages)) {
    std::int64_t prior_released_pages = 0;
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
        if (release_request_slot) {
            if (slot < *release_request_slot) prior_released_pages += static_cast<std::int64_t>(request.kv_pages.size());
            if (slot != *release_request_slot) continue;
        }
        released_lengths_.push_back(length);
        released_tokens_ += length;
        released_pages_ += static_cast<std::int64_t>(request.kv_pages.size());
    }

    // BatchInput describes the state before Prepare. Publication follows all
    // reservations in this batch, including requests which have not finished.
    free_pages_before_release_ = free_pages_before_release_ - new_pages_ + prior_released_pages;
}

} // namespace markov
