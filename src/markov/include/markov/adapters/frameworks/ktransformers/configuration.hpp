#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace markov {

struct KTransformersConfiguration {
    std::string backend_type;
    std::uint32_t tp;
    std::uint32_t max_batch_size;
    std::uint32_t chunk_size;
    std::uint32_t cpu_infer;
    std::uint32_t cache_lens;
    std::string sched_strategy;
    bool use_cuda_graph;

    std::filesystem::path gguf_path;
    std::filesystem::path optimize_config_path;
};

} // namespace markov
