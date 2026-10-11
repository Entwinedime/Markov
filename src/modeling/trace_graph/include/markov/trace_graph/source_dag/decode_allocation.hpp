#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace markov::trace_graph::core {
class DagGraph;
}

namespace markov::trace_graph::modules::hicache {
struct HiCachePhaseObservationAudit;
namespace model {
struct HiCacheReplayFact;
}
} // namespace markov::trace_graph::modules::hicache

namespace markov::trace_graph::source_dag {

using ObservedDecodeContexts = std::map<std::pair<std::string, std::string>, std::map<uint64_t, uint64_t>>;

struct DecodeOperationContext {
    std::string pid, request;
    uint64_t iteration, context_tokens;
    bool operator==(const DecodeOperationContext &) const = default;
};
using DecodeOperatorContexts = std::map<size_t, DecodeOperationContext>;

[[nodiscard]] DecodeOperatorContexts decode_operator_contexts(const modules::hicache::HiCachePhaseObservationAudit & phases,
                                                              const ObservedDecodeContexts & contexts);

/** Observed context tokens keyed by (pid, request), then zero-based round.
 * Only complete, unambiguous single-request non-speculative observations are
 * included. An absent request is unavailable, not a zero-length context.
 */
[[nodiscard]] ObservedDecodeContexts observe_decode_contexts(const core::DagGraph & source_graph,
                                                             const modules::hicache::HiCachePhaseObservationAudit & phases);

/** Source request/round boundaries from approved workload paths and scalar
 * observations. Reads the original DAG only, without target state or costs.
 * Missing, speculative or ambiguous rounds fail rather than becoming empty
 * allocation actions. Timestamps are source microseconds, not target timing.
 */
[[nodiscard]] std::vector<modules::hicache::model::HiCacheReplayFact>
    observe_decode_allocations(const core::DagGraph & source_graph, std::span<const modules::hicache::model::HiCacheReplayFact> facts,
                               const modules::hicache::HiCachePhaseObservationAudit & phases, uint64_t begin_us, uint64_t end_us);

} // namespace markov::trace_graph::source_dag
