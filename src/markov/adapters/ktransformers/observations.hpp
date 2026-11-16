#pragma once

#include "markov/adapters/source.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace markov::adapters::ktransformers {

struct MoeRegion {
    std::size_t layer;
    std::vector<std::size_t> expert_counts;
    trace_graph::planning::Work work;
    std::size_t source_cost;
};

struct Batch {
    std::string request;
    std::size_t iteration;
    trace_graph::source::Time begin_ns, end_ns;
    std::vector<MoeRegion> layers;
    std::size_t residual_cost;
    std::size_t frontend_cost = 0;
};

struct KtSource {
    PreparedSource source;
    std::vector<Batch> batches;
    std::vector<std::size_t> request_tails;
};

trace_graph::planning::Work moe_work(const std::string & phase, const std::vector<std::size_t> & counts, std::size_t query_tokens, std::size_t resident,
                                     std::size_t threads, std::size_t pools);
KtSource read_source(trace_graph::source::ProfileManifest manifest, const nlohmann::json & captured);

} // namespace markov::adapters::ktransformers
