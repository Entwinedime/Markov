#pragma once
#include "markov/trace_graph/cost/operator_model.hpp"
#include "markov/trace_graph/modules/hicache/model/replay.hpp"
#include "markov/trace_graph/simulation/topological_simulator.hpp"
#include <functional>
#include <set>

namespace markov::trace_graph::modules::hicache::runtime {

struct QueueConfirmationCoverage {
    size_t load_rounds = 0, write_rounds = 0, storage_rounds = 0;
    size_t replaced_rounds = 0, partial_window_rounds = 0;
};

struct WriteConfirmationWork {
    size_t round;
    int rank;
    HiCacheFact fact;
    uint64_t confirmed_writes;
};

/** Nonblocking load/write ACKs and storage drains outside replaced wait regions.
 * Source observations supply exact sample/return sites, never target counts.
 * At MIN entry sample the live state. Load/storage publish the common old
 * prefix at outer return; a write-work owner publishes each acknowledged
 * batch at its generated return, before asynchronous event cleanup. The caller
 * must compose the corresponding communication
 * dependencies after bind(). Checks started by the HTTP endpoint retain their
 * complete MIN and outer return, including observed causal-tail children.
 * They do not extend that endpoint. A verified idle barrier or an observed
 * empty load/write setup can supply a missing outer head prefix when all MIN
 * entries are inside the window and the live target sample remains empty.
 * Other missing prefixes/partial MIN rounds are reported, not executed. Every
 * admitted return needs a real CPU boundary.
 */
class HiCacheQueueConfirmations {
public:
    explicit HiCacheQueueConfirmations(cost::OperatorCostProvider & provider) : operator_provider_(provider) {}
    // Called after MIN, before state publication. The caller owns source-tail
    // replacement and returns the last node of the corresponding target work.
    // When provided, it also owns per-batch acknowledgement publication; the
    // outer return must not acknowledge those batches a second time.
    using WriteWork = std::function<std::optional<size_t>(const WriteConfirmationWork &, simulation::FutureDag &)>;
    void bind(core::DagGraph & graph, model::HiCacheModelReplay & replay, uint64_t begin_us, uint64_t end_us,
              const std::vector<core::TraceEvent> & replaced = {}, WriteWork write_work = {}, std::optional<uint64_t> idle_since_us = std::nullopt);
    void replace_load_tails(core::DagGraph & graph);
    void advance(size_t node, uint64_t absolute_time_us, simulation::FutureDag & future);
    [[nodiscard]] const QueueConfirmationCoverage & coverage() const { return coverage_; }

private:
    struct Sample {
        uint64_t loads = 0, writes = 0;
        model::HiCachePrefetchQueueSizes storage;
    };
    struct Check {
        bool storage = false;
        bool write = false;
        std::map<int, HiCacheFact> facts;
        std::map<int, Sample> samples;
        std::set<int> empty_prefix_ranks;
        std::map<int, size_t> return_nodes;
        std::map<int, size_t> sample_nodes, work_nodes;
    };
    enum class Phase { Sample, Work, Apply };
    struct Point {
        size_t check;
        int rank;
        Phase phase;
    };
    std::vector<Check> checks_;
    std::map<size_t, std::vector<Point>> at_;
    model::HiCacheState * state_ = nullptr;
    QueueConfirmationCoverage coverage_;
    WriteWork write_work_;
    cost::OperatorCostProvider & operator_provider_;
    std::map<std::pair<std::string, std::string>, std::pair<double, double>> load_tail_remainders_;
    std::map<size_t, std::string> load_tail_lanes_;
};

} // namespace markov::trace_graph::modules::hicache::runtime
