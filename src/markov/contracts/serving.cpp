#include "markov/contracts/serving.hpp"

#include <stdexcept>

namespace markov {

ServingSettings::ServingSettings(std::int64_t requests, std::int64_t tokens, std::int64_t page, std::int64_t prefill)
    : max_running_requests(requests),
      max_total_tokens(tokens),
      page_size(page),
      max_prefill_tokens(prefill) {
    if (requests <= 0 || page <= 0 || tokens < page || prefill <= 0)
        throw std::invalid_argument("serving needs positive request/prefill capacity and at least one KV page");
}

// Prefill can contribute several tokens per member; Decode contributes one.
std::int64_t Batch::new_tokens() const {
    std::int64_t result = 0;
    for (const auto & member : members) result += member.new_tokens;

    return result;
}

} // namespace markov
