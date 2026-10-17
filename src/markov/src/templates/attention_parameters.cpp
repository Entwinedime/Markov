#include "markov/templates/attention_parameters.hpp"

#include <array>
#include <utility>

namespace markov {

nlohmann::json AttentionParameterRule::describe_parameters() const {
    static constexpr std::array operations{ "cache_write", "prefill", "decode" };
    auto result = shapes_.describe_parameters();
    result["rule"] = "attention";
    result["value_work"] = "attention";
    result["geometry"] = {
        {        "operation", operations[static_cast<std::size_t>(workload_.operation)] },
        {      "query_heads",                                     workload_.query_heads },
        {         "kv_heads",                                        workload_.kv_heads },
        {         "head_dim",                                        workload_.head_dim },
        { "page_size_tokens",                                workload_.page_size_tokens },
        {      "cache_pages",                                     workload_.cache_pages }
    };
    result["sequence_source"] = "target_request_input_context_and_reserved_kv";
    return result;
}

SourceKernel AttentionParameterRule::evaluate(const SourceKernel & source, const BatchInput & input) const {
    SourceKernel target = shapes_.evaluate(source, input);
    auto workload = workload_;
    for (const auto & request : input.requests)
        workload.sequences.push_back({ request.input_tokens, request.context_tokens, request.pool_slot, request.kv_pages });

    target.attention = std::move(workload);
    return target;
}

} // namespace markov
