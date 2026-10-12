#pragma once

#include "markov/trace_graph/structure/operation.hpp"

#include <map>
#include <span>
#include <string_view>
#include <vector>

namespace markov::trace_graph::structure::hicache {

struct PrefetchQueryRankWork {
    OperationWork before_sample, sample_to_min, min_to_apply, agreement;
    std::vector<int> agreement_entries;
};

/** SGLang storage-query worker: sample local hits, MIN across participants,
 * then apply the agreed count. Participants come from the execution group,
 * never from a measurement document. Descriptors contain no durations.
 * The initial cost representation preserves measured coarse intervals;
 * neither a local interval nor the collective remainder is pure CPU service.
 */
[[nodiscard]] std::map<int, PrefetchQueryRankWork> prefetch_query_work(std::span<const int> participants);

// Whole local control intervals, including their residual waiting. These are
// not thread CPU service measurements, even when probe correction is applied.
[[nodiscard]] OperationWork prefetch_check_interval(std::string_view role, int rank, size_t participants);
[[nodiscard]] OperationWork prefetch_check_collective(bool completion, int rank, size_t participants, std::string_view timing_scope);

} // namespace markov::trace_graph::structure::hicache
