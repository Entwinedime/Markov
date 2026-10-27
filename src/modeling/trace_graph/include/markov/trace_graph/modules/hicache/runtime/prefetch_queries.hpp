#pragma once
#include "markov/trace_graph/modules/hicache/model/prefetch_execution.hpp"
#include "markov/trace_graph/modules/hicache/model/prefetch_queue_execution.hpp"
#include "markov/trace_graph/modules/hicache/model/replay.hpp"
#include "markov/trace_graph/modules/hicache/prefetch_worker_observation.hpp"
#include "markov/trace_graph/structure/hicache/io_completion.hpp"
#include <memory>

namespace markov::trace_graph::cost {
class OperatorCostProvider;
}

namespace markov::trace_graph::modules::hicache::runtime {

/** Whole-window query FIFO and independent payload worker. Source identities
 * bind resources; independent database models supply generated query costs.
 * Bind before other control rewrites; start before the first candidate. The
 * caller applies state facts before advance() at the same execution boundary.
 * Replay must outlive this coordinator.
 */
class HiCachePrefetchQueries {
public:
    void bind(core::DagGraph & graph, model::HiCacheModelReplay & replay, cost::OperatorCostProvider & costs, std::map<size_t, std::vector<size_t>> & facts_at,
              const structure::hicache::IoCompletionProvider & completion);
    void start(simulation::FutureDag & future);
    void advance(size_t node, uint64_t absolute_time_us, simulation::FutureDag & future);

private:
    struct Query {
        std::map<int, HiCacheFact> facts;
        CpuRankNodes entries;
        std::unique_ptr<model::HiCachePrefetchQueueExecution> execution;
    };
    std::vector<Query> queries_;
    model::HiCacheState * state_ = nullptr;
    std::unique_ptr<model::HiCachePrefetchExecution> worker_;
};

} // namespace markov::trace_graph::modules::hicache::runtime
