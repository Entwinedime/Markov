#pragma once
#include "markov/trace_graph/modules/hicache/cpu_collectives.hpp"

namespace markov::trace_graph::modules::hicache {

struct PrefetchQueryObservation {
    size_t observation = 0;
    std::optional<size_t> enqueue, release; // runtime observation IDs
    std::optional<size_t> candidate_fact;   // source fact identity, not an executable node
    std::vector<size_t> agreement_rounds;
    std::string issue;
};

struct HostReleaseObservation {
    size_t observation = 0;
    std::string owner, request_id, operation_id;
    std::optional<size_t> query; // index in queries, not an observation ID
    std::optional<size_t> parent_fact;
    std::optional<uint64_t> pages;
    std::string issue;
};

struct StorageDrainObservation {
    size_t observation = 0;
    std::vector<size_t> agreement_rounds;
    std::optional<uint64_t> revoked, backups, released_pages;
    bool local_shutdown = false;
    std::string issue;
};

struct PrefetchWorkerObservations {
    std::vector<PrefetchQueryObservation> queries;
    std::vector<HostReleaseObservation> releases;
    std::vector<StorageDrainObservation> drains;
    std::map<std::string, size_t> issues;
};

/** Source attribution only. Thread order connects query -> MIN -> release and
 * worker return -> release; completion/abort use explicit call context.
 * Round membership is checked separately from unavailable Torch worker costs.
 * Observed counts and timestamps must not be reused as target state decisions.
 */
[[nodiscard]] PrefetchWorkerObservations observe_prefetch_workers(const patch::HiCacheSourceDagIndex & source, const CpuCollectiveObservation & collectives);

} // namespace markov::trace_graph::modules::hicache
