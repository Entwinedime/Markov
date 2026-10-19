#include "markov/templates/model_parameters.hpp"

namespace markov {

SourceKernel ModelParameterRule::evaluate(const SourceKernel & source, const BatchInput & input) const {
    SourceKernel target = shapes_.evaluate(source, input);

    if (workload_ == ModelWorkload::SequenceMaximum) target.value_dependencies.push_back("sequence_length_max");
    if (workload_ == ModelWorkload::SequenceLengths) target.value_dependencies.push_back("sequence_lengths");
    if (workload_ == ModelWorkload::PageTable) target.value_dependencies.push_back("page_table");
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
