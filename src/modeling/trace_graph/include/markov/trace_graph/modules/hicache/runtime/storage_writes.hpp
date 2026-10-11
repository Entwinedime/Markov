#pragma once

#include "markov/trace_graph/cost/hicache_io.hpp"
#include "markov/trace_graph/modules/hicache/model/state.hpp"
#include "markov/trace_graph/simulation/topological_simulator.hpp"

#include <map>
#include <string>
#include <utility>

namespace markov::trace_graph::modules::hicache::runtime {

/** Executes storage submissions produced by host publication. State supplies
 * batches and resource identity, not service estimates. Generate the complete
 * batch sequence, bind its costs, then emit it through FutureDag. Only the final DAG
 * return makes storage readable; background work is not an HTTP endpoint.
 */
class HiCacheStorageWrites {
public:
    HiCacheStorageWrites(model::HiCacheState & state, const frontend::HiCacheConfig & config)
        : state_(state),
          completion_(cost::io_completion_provider(config.io_cost)),
          page_bytes_(config.kv_bytes_per_page) {}

    void advance(size_t node, uint64_t absolute_time_us, simulation::FutureDag & future);

private:
    struct BatchEvent {
        std::string scope, operation;
        size_t batch;
        bool completed;
    };
    model::HiCacheState & state_;
    structure::hicache::IoCompletionProvider completion_;
    uint64_t page_bytes_;
    std::map<std::string, size_t> lane_tail_;
    std::map<size_t, std::pair<std::string, std::string>> returns_;
    std::map<size_t, BatchEvent> batch_events_;
};

} // namespace markov::trace_graph::modules::hicache::runtime
