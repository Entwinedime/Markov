#pragma once
#include "markov/trace_graph/modules/hicache/model/state.hpp"
#include "markov/trace_graph/modules/hicache/phase_observation.hpp"
#include "markov/trace_graph/source_dag/call_regions.hpp"
#include "markov/trace_graph/source_dag/device_collectives.hpp"

namespace markov::trace_graph::modules::hicache::model {

struct HiCacheReplayFact {
    HiCacheFact fact;
    HiCacheFactRole role = HiCacheFactRole::Unknown;
    bool consumed = false;
    bool started = false;
};

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
    /** Borrow the already joined immutable observation for a request/rank.
     * Call only for admitted phases without input blockers. The reference
     * belongs to this replay; target work never overwrites its geometry.
     */
    [[nodiscard]] const HiCachePhaseObservation & source_phase(const std::string & pid, const std::string & request_id) const;
    /** Prepared from the immutable source before any target mutation. Missing
     * metadata remains in the audit; these are not generated target calls.
     */
    [[nodiscard]] const source_dag::AttentionCallAudit & source_attention_calls() const { return source_attention_calls_; }
    [[nodiscard]] const source_dag::AttentionMetadataAudit & source_metadata_calls() const { return source_metadata_calls_; }
    [[nodiscard]] const source_dag::DenseCallAudit & source_dense_calls() const { return source_dense_calls_; }
    [[nodiscard]] const source_dag::DeviceCollectiveAudit & source_device_collectives() const { return source_device_collectives_; }
    [[nodiscard]] const HiCachePhaseObservation & source_phase(size_t index) const { return source_phases_.observations.at(index); }
    [[nodiscard]] HiCacheModelResult finish();

private:
    HiCacheReplayFact & continue_lifecycle(size_t fact_id, uint64_t timestamp_us, size_t execution_node);
    HiCacheState state_;
    HiCachePhaseObservationAudit source_phases_;
    source_dag::AttentionCallAudit source_attention_calls_;
    source_dag::AttentionMetadataAudit source_metadata_calls_;
    source_dag::DenseCallAudit source_dense_calls_;
    source_dag::DeviceCollectiveAudit source_device_collectives_;
    std::map<std::pair<std::string, std::string>, size_t> source_phase_positions_;
    HiCacheModelResult result_;
    std::vector<HiCacheReplayFact> facts_;
    std::map<size_t, size_t> fact_positions_;
    std::optional<uint64_t> last_time_;
    bool execution_prefetch_control_ = false;
    bool finished_ = false;
};

} // namespace markov::trace_graph::modules::hicache::model
