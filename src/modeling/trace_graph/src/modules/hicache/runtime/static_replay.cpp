#include "markov/trace_graph/modules/hicache/runtime/static_replay.hpp"

#include "markov/trace_graph/modules/hicache/model/replay.hpp"
#include "markov/trace_graph/modules/hicache/runtime/io_completion.hpp"

namespace markov::trace_graph::modules::hicache::runtime {

model::HiCacheModelResult replay_hicache_static(core::DagGraph & graph, const frontend::HiCacheConfig & config,
                                                const model::HiCacheFactClock & execution_clock) {
    model::HiCacheModelReplay replay(graph,
                                     config,
                                     runtime::io_completion_provider(config.io_cost),
                                     config.io_cost.storage_batch_pages,
                                     config.io_cost.resource_lanes,
                                     false,
                                     execution_clock);
    for (const auto & item : replay.facts()) replay.apply(item.fact.source_node_id, item.fact.ts);

    return replay.finish();
}

} // namespace markov::trace_graph::modules::hicache::runtime
