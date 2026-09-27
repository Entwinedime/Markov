/** Source-supported CPU communication calls used by HiCache scheduling. */
#pragma once

#include "markov/trace_graph/modules/hicache/patch/source_dag_index.hpp"

namespace markov::trace_graph::modules::hicache {

struct CpuCollectiveCall {
    size_t observation = 0; // Index in graph.runtime_observations(), not an executable node.
    int rank = -1;
    patch::HiCacheTimingIntervalOwnership cpu;
    std::optional<size_t> submission, worker;
    std::string issue;
};

struct CpuCollectiveRound {
    std::string group;
    std::vector<int> members;
    uint64_t index = 0;
    std::vector<CpuCollectiveCall> calls;
    std::string issue;
};

struct CpuCollectiveObservation {
    std::vector<CpuCollectiveRound> rounds;
    std::map<std::string, size_t> issues;
};

/** Read-only identity and containment checks; missing roles remain missing. */
[[nodiscard]] CpuCollectiveObservation observe_cpu_collectives(const patch::HiCacheSourceDagIndex & source);


} // namespace markov::trace_graph::modules::hicache
