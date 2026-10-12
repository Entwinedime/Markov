#pragma once
#include "markov/trace_graph/modules/hicache/runtime/lifecycle_observation.hpp"
#include <set>

namespace markov::trace_graph::modules::hicache::runtime {

class HiCacheLifecycleCalls {
public:
    void bind(core::DagGraph & graph, const model::HiCacheModelReplay & replay, uint64_t begin, uint64_t end);
    [[nodiscard]] bool owns(size_t fact) const { return owners_.contains(fact); }
    // Updated owners; only the outer return makes their replay fact consumed.
    std::vector<size_t> advance(size_t node, uint64_t time, model::HiCacheModelReplay & replay) const;

private:
    enum class Kind { Entry, Ready, InsertReturn, Return };
    struct Action { Kind kind; size_t owner; uint64_t tokens; };
    std::set<size_t> owners_;
    std::map<size_t, std::vector<Action>> actions_;
};

} // namespace markov::trace_graph::modules::hicache::runtime
