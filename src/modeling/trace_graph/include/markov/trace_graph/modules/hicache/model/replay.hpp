#pragma once
#include "markov/trace_graph/modules/hicache/model/state.hpp"
#include "markov/trace_graph/modules/hicache/phase_observation.hpp"

namespace markov::trace_graph::modules::hicache::model {

struct HiCacheReplayFact {
    HiCacheFact fact;
    HiCacheFactRole role = HiCacheFactRole::Unknown;
    bool consumed = false;
    bool started = false;
};

/** Derive allocation actions from base observations and approved input paths.
 * No target data, state mutation or costs. */
[[nodiscard]] std::vector<HiCacheReplayFact> observe_decode_allocations(
    const core::DagGraph & graph, std::span<const HiCacheReplayFact> facts,
    const HiCachePhaseObservationAudit & phases, uint64_t begin, uint64_t end);

/** One state for an entire formal window. Parse source identities and capture
 * phase observations before simulation; apply formal/tail facts only when their
 * execution boundary is reached, using the common absolute clock.
 *
 * execution_prefetch_control requires explicit query/stop/publication/drain
 * events on state(). It neither looks ahead to future extend times nor settles
 * unfinished background work at finish(). The existing batch entry uses the
 * same implementation with its source-clock control approximation.
 */
class HiCacheModelReplay {
public:
    HiCacheModelReplay(const core::DagGraph & graph, const frontend::HiCacheConfig & config, bool execution_prefetch_control,
                       const HiCacheFactClock & projected_clock = {});
    [[nodiscard]] const std::vector<HiCacheReplayFact> & facts() const { return facts_; }
    /** Formal identity lookup; borrowed until preparation changes the fact list. */
    [[nodiscard]] const HiCacheReplayFact & fact(size_t id) const { return facts_.at(fact_positions_.at(id)); }
    [[nodiscard]] HiCacheState & state() { return state_; }
    /** Join base allocation boundaries to approved workload inputs before any
     * formal fact executes. Missing decode observations are not zero work. */
    void prepare_decode_allocations(const core::DagGraph & graph, uint64_t begin, uint64_t end);
    void apply(size_t fact_id, uint64_t timestamp_us, std::optional<size_t> execution_node = std::nullopt,
               HiCacheLifecycleExecution lifecycle = HiCacheLifecycleExecution::Immediate);
    void advance_lifecycle(size_t fact_id, uint64_t timestamp_us, size_t execution_node, uint64_t ready_tokens);
    void return_lifecycle(size_t fact_id, uint64_t timestamp_us, size_t execution_node);
    void resume(size_t fact_id, uint64_t timestamp_us, size_t execution_node);
    /** Append costs for newly admitted phase work without replaying earlier requests.
     * The borrowed ledger belongs to this replay and changes on the next call.
     * With no blockers, prefills and decodes have matching positions. Does not
     * finalize state or build I/O effects; finish() produces the complete result. */
    [[nodiscard]] const HiCachePhaseWorkLedger & current_phase_work();
    [[nodiscard]] HiCacheModelResult finish();

private:
    HiCacheReplayFact & continue_lifecycle(size_t fact_id, uint64_t timestamp_us, size_t execution_node);
    HiCacheState state_;
    HiCachePhaseObservationAudit source_phases_;
    std::map<std::pair<std::string, std::string>, size_t> source_phase_positions_;
    HiCacheModelResult result_;
    std::vector<HiCacheReplayFact> facts_;
    std::map<size_t, size_t> fact_positions_;
    std::optional<uint64_t> last_time_;
    bool execution_prefetch_control_ = false;
    bool finished_ = false;
};

} // namespace markov::trace_graph::modules::hicache::model
