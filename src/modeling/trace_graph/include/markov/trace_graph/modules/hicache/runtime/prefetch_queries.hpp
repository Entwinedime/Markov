#pragma once
#include "markov/trace_graph/modules/hicache/model/prefetch_execution.hpp"
#include "markov/trace_graph/modules/hicache/model/prefetch_queue_execution.hpp"
#include "markov/trace_graph/modules/hicache/model/replay.hpp"
#include "markov/trace_graph/modules/hicache/prefetch_worker_observation.hpp"
#include <functional>
#include <memory>

namespace markov::trace_graph::modules::hicache::runtime {

struct PrefetchQueryTemplate {
    model::PrefetchQueueTiming timing;
    std::map<int, size_t> observations; // rank -> base runtime query observation
    std::string issue;
    std::string calibration_manifest;
};

/** First complete query in the declared base window, not an error-selected
 * donor. CPU/dispatch and collective envelopes include measured scheduling and
 * probe overhead. They are a coarse branch template, not pure service costs.
 */
[[nodiscard]] PrefetchQueryTemplate observe_prefetch_query_template(const core::DagGraph & graph, const CpuCollectiveObservation & collectives,
                                                                    const PrefetchWorkerObservations & workers, uint64_t begin_us, uint64_t end_us);

struct PrefetchQueryEntry {
    std::string request_id;
    int rank;
    uint64_t semantic_end_us, submission_us;
};

struct PrefetchQueryReturn {
    std::string request_id;
    int rank;
    uint64_t timestamp_us, hit_pages;
    bool operation, payload;
};

/** Whole-window query FIFO and independent payload worker. Uses only the
 * already-built base graph and target state; no manifests, files or JSON.
 * Bind before other control rewrites; start before the first candidate. The
 * caller applies state facts before advance() at the same execution boundary.
 * Replay and config must outlive this coordinator. Diagnostics are optional.
 */
class HiCachePrefetchQueries {
public:
    using ReturnObserver = std::function<void(const PrefetchQueryReturn &)>;
    void bind(core::DagGraph & graph, model::HiCacheModelReplay & replay, const frontend::HiCacheConfig & config,
              std::map<size_t, std::vector<size_t>> & facts_at, uint64_t begin_us, uint64_t end_us, ReturnObserver observer = {});
    void start(simulation::FutureDag & future);
    void advance(size_t node, uint64_t absolute_time_us, simulation::FutureDag & future);
    [[nodiscard]] const PrefetchQueryTemplate & source_template() const { return template_; }
    [[nodiscard]] const std::vector<PrefetchQueryEntry> & entries() const { return entries_; }

private:
    struct Query {
        std::map<int, HiCacheFact> facts;
        CpuRankNodes entries;
        std::unique_ptr<model::HiCachePrefetchQueueExecution> execution;
    };
    PrefetchQueryTemplate template_;
    std::vector<PrefetchQueryEntry> entries_;
    std::vector<Query> queries_;
    model::HiCacheState * state_ = nullptr;
    std::unique_ptr<model::HiCachePrefetchExecution> worker_;
    ReturnObserver observer_;
};

} // namespace markov::trace_graph::modules::hicache::runtime
