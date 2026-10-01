#pragma once
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
 * At MIN entry sample the live state; publish the common old prefix at the
 * outer return, after optional target work emitted at MIN return. The caller
 * must compose the corresponding communication
 * dependencies after bind(). Complete in-window MIN rounds retain their outer
 * return even past the HTTP endpoint. A verified idle barrier can supply a
 * missing head prefix, provided the live target sample remains empty; other
 * missing prefixes/partial MIN rounds are reported, not executed. Every
 * admitted return needs a real CPU boundary.
 */
class HiCacheQueueConfirmations {
public:
    // Called after MIN, before state publication. The caller owns source-tail
    // replacement and returns the last node of the corresponding target work.
    using WriteWork = std::function<std::optional<size_t>(const WriteConfirmationWork &, simulation::FutureDag &)>;
    void bind(core::DagGraph & graph, model::HiCacheModelReplay & replay, uint64_t begin_us, uint64_t end_us,
              const std::vector<core::TraceEvent> & replaced = {}, WriteWork write_work = {}, std::optional<uint64_t> idle_since_us = std::nullopt,
              bool generate_load_tails = false);
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
        std::set<int> idle_prefix_ranks;
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
    struct LoadTailCost {
        double empty_cpu = 0, empty_gap = 0, batch_cpu = 0, batch_gap = 0;
        bool batch_proxy = false;
        std::pair<double, double> remainder{};
    };
    std::map<std::pair<std::string, std::string>, LoadTailCost> load_tail_costs_;
    std::map<size_t, std::string> load_tail_lanes_;
};

} // namespace markov::trace_graph::modules::hicache::runtime
