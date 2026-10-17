#include "markov/templates/model_parameters.hpp"

#include <algorithm>
#include <array>

namespace markov {

nlohmann::json ModelParameterRule::describe_parameters() const {
    static constexpr std::array workloads{ "none",      "sequence_length_max", "sequence_lengths",    "page_table",
                                           "positions", "last_positions",      "logits_token_indices" };
    auto result = shapes_.describe_parameters();
    result["rule"] = "model";
    result["value_work"] = workloads[static_cast<std::size_t>(workload_)];
    result["partial_operands"] = partial_operands_;
    return result;
}

SourceKernel ModelParameterRule::evaluate(const SourceKernel & source, const BatchInput & input) const {
    SourceKernel target = shapes_.evaluate(source, input);

    if (workload_ == ModelWorkload::SequenceMaximum) {
        std::int64_t maximum = 0;
        for (const auto & request : input.requests) maximum = std::max(maximum, static_cast<std::int64_t>(request.context_tokens) + request.input_tokens);
        target.sequence_length_max = maximum;
    }
    if (workload_ == ModelWorkload::SequenceLengths) {
        target.sequence_lengths.emplace();
        for (const auto & request : input.requests)
            target.sequence_lengths->push_back(static_cast<std::int64_t>(request.context_tokens) + request.input_tokens);
    }
    if (workload_ == ModelWorkload::PageTable) {
        target.page_table = PageTableWorkload{ shapes_.page_size_tokens(), {} };
        for (const auto & request : input.requests)
            target.page_table->sequences.push_back({ request.input_tokens, request.context_tokens, request.pool_slot, request.kv_pages });
    }
    if (workload_ == ModelWorkload::Positions) {
        target.positions.emplace();
        for (const auto & request : input.requests) target.positions->sequences.push_back({ request.context_tokens, request.input_tokens });
    }
    if (workload_ == ModelWorkload::LastPositions) {
        target.positions.emplace();
        // Sampling uses one final position per request, not every query token.
        for (const auto & request : input.requests)
            target.positions->sequences.push_back({ static_cast<std::int64_t>(request.context_tokens) + request.input_tokens - 1, 1 });
    }
    if (workload_ == ModelWorkload::LogitsIndices) {
        target.logits_token_indices.emplace();
        std::int64_t end = 0;
        for (const auto & request : input.requests) {
            end += request.input_tokens;
            target.logits_token_indices->push_back(end - 1);
        }
    }
    return target;
}

} // namespace markov
