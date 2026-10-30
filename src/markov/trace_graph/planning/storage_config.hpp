#pragma once

#include "markov/trace_graph/graph/graph.hpp"
#include "markov/trace_graph/planning/work.hpp"

#include <cstddef>
#include <optional>

namespace markov::trace_graph::planning {

enum class PrefetchPolicy { Complete, BestEffort, Timeout };

struct StorageConfig {
    std::optional<std::size_t> capacity_pages;
    PrefetchPolicy policy;
    std::size_t threshold_tokens;
    double prefetch_limit_pages;
    graph::Time timeout_base_ns, timeout_per_ki_token_ns, timeout_max_ns;
};

struct StorageBackend {
    std::size_t batch_pages;
    WorkKind allocate, read_page, copy_publish, read_return;
    WorkKind gather, batch_prepare, new_page, existing_page;
};

struct PrefetchResult {
    bool examined = false, ready = false;
    std::size_t requested_pages = 0, queried_pages = 0, published_pages = 0, accepted_pages = 0, inserted_pages = 0;
    bool stopped_early = false, worker_done = false, rebased_suffix = false;
    std::optional<graph::Time> issued_ns, first_check_ns, accepted_ns, worker_finished_ns;
};

struct StorageStats {
    // Rank-prefixed counters sum physical services; capacity peaks are per rank.
    std::size_t queries = 0, prefetches = 0, rank_read_pages = 0, rank_copied_pages = 0, rank_rejected_copies = 0;
    std::size_t writes = 0, rank_new_pages = 0, rank_existing_pages = 0, rank_disk_evictions = 0, host_evicted_pages = 0;
    std::size_t peak_reserved_pages = 0, peak_rank_stored_pages = 0;
};

} // namespace markov::trace_graph::planning
