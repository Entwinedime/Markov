#pragma once
#include "markov/trace_graph/modules/hicache/cpu_collectives.hpp"
#include "markov/trace_graph/simulation/topological_simulator.hpp"
#include <set>

namespace markov::trace_graph::modules::hicache {
using CpuRankTimes = std::map<int, uint64_t>;

struct CpuCollectiveCallTiming {
    std::vector<int> entry_ranks;
    uint64_t before_submit = 0, submission = 0, dispatch = 0;
    uint64_t worker_remainder = 0, after_join = 0;
    // Alternative evidence: whole synchronous call minus waiting for peer call
    // entry. Includes unseparated CPU/dispatch/probe time; native fields unused.
    std::optional<uint64_t> envelope_remainder_us;
};

struct CpuCollectiveTiming {
    std::map<int, CpuCollectiveCallTiming> calls;
    std::string issue;
};

/** Source-measured delays/remainders, not pure calibrated communication service.
 * Caller before/submit/after work includes bound same-source CPU correction.
 * Dispatch and worker remainder retain observed cross-thread timing; they do
 * not inherit correction from the caller lane. All durations are microseconds.
 * Shared by static DAG mutation and execution-driven collective generation.
 */
[[nodiscard]] CpuCollectiveTiming observe_cpu_collective_timing(const core::DagGraph & graph, const CpuCollectiveRound & round);
/** Explicit coarse timing for synchronous all-reduce without worker coverage.
 * Never selected by the native-worker observer or its source rewrite planner.
 * Retain the same membership/message signature when reusing this template.
 */
[[nodiscard]] CpuCollectiveTiming observe_cpu_all_reduce_envelope(const core::DagGraph & graph, const CpuCollectiveRound & round);
using CpuRankNodes = std::map<int, size_t>;
/** Reserve dependency gates, then submit each rank at its own execution entry.
 * Submit before that rank's entry completes. Reservation creates no cost;
 * callers that return locally must not prepare a collective in the first place.
 */
class CpuCollectiveExecution {
public:
    explicit CpuCollectiveExecution(CpuCollectiveTiming timing);
    CpuRankNodes prepare(const CpuRankNodes & entries, simulation::FutureDag & future);
    size_t submit(int rank, size_t call_entry, simulation::FutureDag & future);

private:
    CpuCollectiveTiming timing_;
    CpuRankNodes owners_, arrivals_, returned_;
    std::set<int> submitted_;
};
/** Materializes the same timing dependencies during execution. Call before the
 * earliest entry (or at that zero-cost boundary); already-running calls cannot
 * be reconstructed retroactively. Entries supply each rank's provenance.
 */
[[nodiscard]] CpuRankNodes append_cpu_collective(const CpuCollectiveTiming & timing, const CpuRankNodes & entries, simulation::FutureDag & future);

} // namespace markov::trace_graph::modules::hicache
