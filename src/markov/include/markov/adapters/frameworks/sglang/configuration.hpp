#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <string>

namespace markov {

struct SGLangConfiguration {
    std::string device;
    std::uint32_t tp_size;
    std::string attention_backend;

    std::uint32_t max_running_requests;
    std::uint32_t max_total_tokens;
    std::int32_t chunked_prefill_size;
    std::uint32_t max_prefill_tokens;
    std::string schedule_policy;

    bool disable_radix_cache;
    bool disable_overlap_schedule;
    bool disable_cuda_graph;
    bool enable_mixed_chunk;
    bool enable_hierarchical_cache;

    static SGLangConfiguration from_parameters(const nlohmann::json & parameters);
    nlohmann::json parameters() const;

    bool operator==(const SGLangConfiguration &) const = default;
};

} // namespace markov
