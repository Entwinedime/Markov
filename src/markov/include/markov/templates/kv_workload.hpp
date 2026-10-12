#pragma once

#include "markov/contracts/control.hpp"
#include "markov/graph/source_graph.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace markov {

// Target work derived from the invocation, never from native physical page IDs.
// The input is borrowed only for the duration of parameter evaluation.
class KvBatchWork {
public:
    KvBatchWork(const BatchInput & input, std::uint32_t page_size_tokens, std::optional<std::size_t> release_request_slot = std::nullopt);
    std::int64_t input_tokens() const { return input_tokens_; }
    std::int64_t new_pages() const { return new_pages_; }
    bool single_token_requests() const { return single_token_requests_; }
    std::int64_t free_pages_before_release() const;
    std::int64_t released_tokens() const { return released_tokens_; }
    std::int64_t released_pages() const { return released_pages_; }
    const std::vector<std::int64_t> & released_lengths() const { return released_lengths_; }
    KvAllocationWorkload allocation() const;
    KvReleaseWorkload release() const;

private:
    const BatchInput & input_;
    std::uint32_t page_size_tokens_;
    std::optional<std::size_t> release_request_slot_;
    std::int64_t prior_released_pages_ = 0;
    std::int64_t input_tokens_ = 0;
    std::int64_t new_pages_ = 0;
    bool single_token_requests_ = true;
    std::int64_t released_tokens_ = 0;
    std::int64_t released_pages_ = 0;
    std::vector<std::int64_t> released_lengths_;
};

} // namespace markov
