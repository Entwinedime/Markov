#pragma once
#include "markov/trace_graph/modules/hicache/cpu_collective_timing.hpp"
#include "markov/trace_graph/modules/hicache/service_model.hpp"
#include <set>

namespace markov::trace_graph::modules::hicache {

struct PrefetchControlCpuInterval {
    std::string role;
    patch::HiCacheTimingIntervalOwnership cpu;
};

/** One source progress call. Observation/round ids refer to the original graph
 * and collective table. stop_call bounds cancellation and its returned snapshot;
 * neither endpoint claims to be the exact internal instruction timestamp.
 * Decisions and completed_tokens verify source attribution, not target progress.
 * CPU intervals retain instrumentation work when an outer control call exists.
 */
struct PrefetchControlObservation {
    size_t progress_fact = 0;
    std::optional<size_t> check, stop, state_round, completion_round;
    std::optional<int> rank;
    std::optional<uint64_t> completed_tokens;
    bool uses_control_envelope = false;
    bool local_return = false; // Proven inactive path, not a missing check probe.
    std::vector<PrefetchControlCpuInterval> cpu;
    std::string issue;
};

[[nodiscard]] std::vector<PrefetchControlObservation> observe_prefetch_control(const patch::HiCacheSourceDagIndex & source,
                                                                               const CpuCollectiveObservation & collectives, std::string_view source_policy);

using PrefetchRequestObservations = std::map<std::string, std::map<int, std::vector<PrefetchControlObservation>>>;
/** Validate selected requests and order each rank's calls. A null filter means
 * all source requests; an empty filter means none. */
[[nodiscard]] PrefetchRequestObservations observe_prefetch_requests(const patch::HiCacheSourceDagIndex & source, const CpuCollectiveObservation & collectives,
                                                                    std::string_view policy, const std::set<std::string> * requests = nullptr);

/** Source measurements, not a target service prediction. Increment return is
 * the publication upper bound; observation ids retain both endpoints. A full
 * successful batch cannot supply the unexecuted cancelled-return branch.
 */
struct PrefetchServiceObservation {
    size_t service_fact = 0;
    std::string request_id;
    std::optional<size_t> read;
    std::vector<size_t> publications;
    PrefetchControlBatch batch{};
    // Actual executed work, including the copy preceding a rejected increment.
    // A cancelled observation is not a full hypothetical publication timeline.
    std::optional<PrefetchBatchExecution> observed_work;
    std::string issue;
};

[[nodiscard]] std::vector<PrefetchServiceObservation> observe_prefetch_services(const patch::HiCacheSourceDagIndex & source);

} // namespace markov::trace_graph::modules::hicache
