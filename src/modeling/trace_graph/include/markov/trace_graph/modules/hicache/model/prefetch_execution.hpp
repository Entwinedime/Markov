#pragma once
#include "markov/trace_graph/modules/hicache/model/state.hpp"
#include "markov/trace_graph/modules/hicache/service_model.hpp"
#include "markov/trace_graph/simulation/topological_simulator.hpp"

#include <deque>

namespace markov::trace_graph::modules::hicache::model {

/** Executes target-issued storage reads using the existing staged service model.
 * Call enqueue after query MIN and advance from the DAG expansion callback.
 * Foreground stop/publication and scheduler drains belong to the caller; this
 * worker only produces physical work and its return. State and config must
 * outlive it; do not reset the formal window while its tasks are outstanding.
 */
class HiCachePrefetchExecution {
public:
    HiCachePrefetchExecution(HiCacheState & state, const frontend::HiCacheConfig & config) : state_(state), config_(config) {}
    // No node for a suppressed/revoked query. Returned completion is a resource
    // boundary, not a foreground dependency or an HTTP endpoint.
    std::optional<size_t> enqueue(const HiCacheFact & candidate, simulation::FutureDag & future);
    void advance(size_t node, uint64_t absolute_time_us, simulation::FutureDag & future);

private:
    enum class Step { Begin, Publication, BatchReturn, Complete };
    struct Event {
        size_t operation;
        Step step;
    };
    struct Execution {
        const HiCachePrefetchOperation * operation;
        size_t completion;
        uint64_t remaining_pages;
        PrefetchControlBatch batch;
        size_t publication = 0;
        bool cancelled = false;
        std::vector<runtime::HiCacheIoBatchSchedule> executed;
    };
    void begin_batch(size_t index, uint64_t time, simulation::FutureDag & future);
    void schedule(size_t index, Step step, uint64_t duration, simulation::FutureDag & future);
    HiCacheState & state_;
    const frontend::HiCacheConfig & config_;
    std::deque<Execution> executions_;
    std::unordered_map<size_t, Event> events_;
    std::unordered_map<std::string, size_t> lane_tail_;
    std::unordered_map<const HiCachePrefetchOperation *, size_t> submitted_;
};

} // namespace markov::trace_graph::modules::hicache::model
