#include "markov/adapters/frameworks/sglang/configuration.hpp"
#include "markov/contracts/json_input.hpp"

#include <nlohmann/json.hpp>

#include <limits>
#include <stdexcept>

namespace markov {

using Json = nlohmann::json;
using input::fields;
using input::integer;
using input::positive;
using input::string_value;

SGLangConfiguration SGLangConfiguration::from_parameters(const Json & p) {
    fields(p,
           { "device",
             "tp_size",
             "attention_backend",
             "max_running_requests",
             "max_total_tokens",
             "chunked_prefill_size",
             "max_prefill_tokens",
             "schedule_policy",
             "disable_radix_cache",
             "disable_overlap_schedule",
             "disable_cuda_graph",
             "enable_mixed_chunk",
             "enable_hierarchical_cache" });

    const auto chunk_size = integer(p.at("chunked_prefill_size"), -1, std::numeric_limits<std::int32_t>::max());
    if (chunk_size == 0) { throw std::invalid_argument("chunked_prefill_size is -1 or a positive integer"); }

    return SGLangConfiguration{ string_value(p.at("device")),
                                positive(p.at("tp_size")),
                                string_value(p.at("attention_backend")),
                                positive(p.at("max_running_requests")),
                                positive(p.at("max_total_tokens")),
                                static_cast<std::int32_t>(chunk_size),
                                positive(p.at("max_prefill_tokens")),
                                string_value(p.at("schedule_policy")),
                                p.at("disable_radix_cache").get<bool>(),
                                p.at("disable_overlap_schedule").get<bool>(),
                                p.at("disable_cuda_graph").get<bool>(),
                                p.at("enable_mixed_chunk").get<bool>(),
                                p.at("enable_hierarchical_cache").get<bool>() };
}

nlohmann::json SGLangConfiguration::parameters() const {
    return {
        {                    "device",                    device },
        {                   "tp_size",                   tp_size },
        {         "attention_backend",         attention_backend },
        {      "max_running_requests",      max_running_requests },
        {          "max_total_tokens",          max_total_tokens },
        {      "chunked_prefill_size",      chunked_prefill_size },
        {        "max_prefill_tokens",        max_prefill_tokens },
        {           "schedule_policy",           schedule_policy },
        {       "disable_radix_cache",       disable_radix_cache },
        {  "disable_overlap_schedule",  disable_overlap_schedule },
        {        "disable_cuda_graph",        disable_cuda_graph },
        {        "enable_mixed_chunk",        enable_mixed_chunk },
        { "enable_hierarchical_cache", enable_hierarchical_cache }
    };
}

} // namespace markov
