#pragma once

#include "markov/contracts/control.hpp"
#include "markov/graph/source_graph.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>

namespace markov {

enum class TransferExtent { FixedAtRequestCount, InputTokens, FinishedSequenceTokens, RequestElements, TransitionRetainedRequests, RunningRetainedRequests };

struct TransferParameterLayout {
    TransferExtent extent;
    std::size_t request_count;
    std::size_t finished_count = 0;
    std::uint64_t element_bytes = 0;

    bool operator==(const TransferParameterLayout &) const = default;

    std::expected<SourceTransfer, std::string> evaluate(const SourceTransfer & source, const BatchInput & input) const;
};

} // namespace markov
