#include "markov/templates/attention_parameters.hpp"

#include <utility>

namespace markov {

SourceKernel AttentionParameterRule::evaluate(const SourceKernel & source, const BatchInput & input) const {
    SourceKernel target = shapes_.evaluate(source, input);
    auto workload = workload_;
    for (const auto & request : input.requests)
        workload.sequences.push_back({ request.input_tokens, request.context_tokens, request.pool_slot, request.kv_pages });

    target.attention = std::move(workload);
    return target;
}

} // namespace markov
