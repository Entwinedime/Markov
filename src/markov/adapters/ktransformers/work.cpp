#include "markov/adapters/ktransformers/work.hpp"

namespace markov::adapters::ktransformers {
namespace {

std::string implementation(const nlohmann::json & configuration, const std::string & mode) {
    return "ktransformers.dense.torch:" + configuration.at("attention_backend").get<std::string>() + ":" + mode
           + ":threads=" + std::to_string(configuration.at("cpu_threads").get<std::size_t>());
}

} // namespace

trace_graph::planning::Work phase_work(const nlohmann::json & configuration, const std::string & phase, std::size_t query, std::size_t context, bool first,
                                       bool emit) {
    const auto q = static_cast<double>(query), c = static_cast<double>(context);
    return {
        {  "inference." + phase, implementation(configuration,                  emit ? "emit" : "cache_only"),         "synchronous_phase_interval" },
        { { "query_tokens", q },      { "context_tokens", c }, { "attention_pairs", q * (2 * c - q + 1) / 2 }, { "first_phase", first ? 1.0 : 0.0 } }
    };
}

trace_graph::planning::Work control_work(const nlohmann::json & configuration, std::size_t input, std::size_t prefills, std::size_t decodes, bool first) {
    return {
        {                    "inference.request_control",implementation(configuration,"control"),"exclusive_request_interval"                  },
        { { "input_tokens", static_cast<double>(input) },
         { "prefill_steps", static_cast<double>(prefills) },
         { "decode_steps", static_cast<double>(decodes) },
         { "first_request", first ? 1.0 : 0.0 } }
    };
}

} // namespace markov::adapters::ktransformers
