#pragma once
#include "markov/trace_graph/modules/hicache/model/input.hpp"
#include "markov/trace_graph/modules/hicache/model/state.hpp"
#include "markov/trace_graph/modules/hicache/phase_observation.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace markov::trace_graph::modules::hicache::model {

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
    // The coordinator supplies completion callbacks for prelude/static work.
    // Numerical models and their construction remain outside replay rules.
    HiCacheModelReplay(const core::DagGraph & graph, const frontend::HiCacheRules & config, structure::hicache::IoCompletionProvider completion,
                       uint64_t storage_batch_pages, frontend::HiCacheIoResourceLanesConfig resource_lanes, bool execution_prefetch_control,
                       const HiCacheFactClock & projected_clock = {});
    [[nodiscard]] const std::vector<HiCacheReplayFact> & facts() const { return facts_; }
    /** Formal identity lookup; borrowed until preparation changes the fact list. */
    [[nodiscard]] const HiCacheReplayFact & fact(size_t id) const { return facts_.at(fact_positions_.at(id)); }
    [[nodiscard]] HiCacheState & state() { return state_; }
    /** Join original source boundaries to approved workload inputs before any
     * formal fact executes. Missing decode observations are not zero work;
     * source rounds identify opportunities, not target page-allocation outcomes.
     */
    void prepare_decode_allocations(const core::DagGraph & source_graph, uint64_t begin_us, uint64_t end_us);
    void apply(size_t fact_id, uint64_t timestamp_us, std::optional<size_t> execution_node = std::nullopt,
               HiCacheLifecycleExecution lifecycle = HiCacheLifecycleExecution::Immediate);
    /** Continue a lookup at its registered admission boundary. Loading
     * uses the target lookup's hit snapshot and current allocation capacity,
     * never the source condition result.
     */
    void admit_load(size_t fact_id, uint64_t timestamp_us, size_t execution_node);
    void advance_lifecycle(size_t fact_id, uint64_t timestamp_us, size_t execution_node, uint64_t ready_tokens);
    void return_lifecycle(size_t fact_id, uint64_t timestamp_us, size_t execution_node);
    void resume(size_t fact_id, uint64_t timestamp_us, size_t execution_node);
    /** Append work and source operator references for newly admitted phases.
     * The borrowed ledger belongs to this replay and changes on the next call.
     * With no blockers, prefills and decodes have matching positions. Does not
     * finalize state or build I/O effects; finish() produces the complete result. */
    [[nodiscard]] const HiCachePhaseWorkLedger & current_phase_work();
    /** Immutable request/round observations. Detailed operator calls belong
     * to source_dag, not the cache state replay. Borrowed until replay ends.
     */
    [[nodiscard]] const HiCachePhaseObservationAudit & source_phases() const { return source_phases_; }
    /** Index into source_phases().observations for an admitted request/rank.
     * Missing or ambiguous identities remain input blockers; no nearest match.
     */
    [[nodiscard]] size_t source_phase_index(const std::string & pid, const std::string & request_id) const {
        return source_phase_positions_.at({ pid, request_id });
    }
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
