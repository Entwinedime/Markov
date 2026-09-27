#pragma once
#include "markov/trace_graph/modules/hicache/model/state.hpp"
#include "markov/trace_graph/modules/hicache/patch/io_operation_ledger.hpp"
#include "markov/trace_graph/simulation/topological_simulator.hpp"
#include <map>
#include <set>

namespace markov::trace_graph::modules::hicache::runtime {

struct HiCacheWriteStreamPosition {
    std::optional<size_t> before, after;
    std::optional<core::DagObservedPoint> submission_site;
};

/** Locate a new Record by CPU submission order on a proven device stream.
 * GPU execution timestamps do not identify the insertion position. Missing,
 * overlapping or cross-thread submission evidence is reported, not guessed.
 */
// stream_node may be an inactive donor: only its rank/lane identify the resource.
// Returned dependency endpoints always belong to the surviving active stream.
[[nodiscard]] HiCacheWriteStreamPosition observe_write_stream_position(const patch::HiCacheSourceDagIndex & source, size_t stream_node, const std::string & pid,
                                                                       const std::string & tid, uint64_t at_us);

/** D2H service and completion, not cache publication or allocation continuation.
 * Source sites retain their CPU submissions, stream readiness and final Record.
 * The caller assigns target operations before those device nodes can start.
 * New branches require an explicit data-ready predecessor; this class does not
 * infer it from timestamps or invent the branch's host submission cost.
 */
class HiCacheWrites {
public:
    explicit HiCacheWrites(model::HiCacheState & state) : state_(state) {}
    void add_source(const patch::HiCacheSourceDagIndex & source, const patch::HiCacheIoOperationRecord & record);
    // Static host replacements can remove or reconnect completion consumers.
    void rebind_consumers(const patch::HiCacheSourceDagIndex & source);
    /** Recover the donor's compute stream through its actual Record -> Wait.
     * Reuse the stream identity, never the donor request's data predecessor. */
    [[nodiscard]] size_t source_start_record(const patch::HiCacheSourceDagIndex & source, size_t source_fact) const;
    [[nodiscard]] size_t source_completion(size_t source_fact) const { return sources_.at(source_fact).completion; }
    /** A null write cancels the source payload, leaving its CPU control intact. */
    std::optional<size_t> submit_source(size_t source_fact, const HiCacheFact & fact, const std::optional<model::HiCacheDeviceWrite> & write,
                                        simulation::FutureDag & future);
    /** Register an expanded host/device call; order its whole write stream,
     * including its leading Wait, and complete only after its final Record. */
    size_t submit_expanded(const HiCacheFact & fact, const model::HiCacheDeviceWrite & write, size_t write_start, size_t completion,
                           simulation::FutureDag & future);
    [[nodiscard]] uint64_t duration(size_t node, uint64_t original) const;
    void advance(size_t node, uint64_t time);
    [[nodiscard]] size_t completed() const { return completed_; }

private:
    struct Source {
        std::vector<std::pair<size_t, uint64_t>> transfers;
        uint64_t bytes = 0;
        size_t completion = 0;
        std::vector<size_t> successors;
        bool submitted = false;
        uint64_t begin_us = 0, end_us = 0;
    };
    struct Completion {
        HiCacheFact fact;
        std::string operation;
    };
    void register_write(const HiCacheFact & fact, const model::HiCacheDeviceWrite & write);
    size_t complete_after(const HiCacheFact & fact, const model::HiCacheDeviceWrite & write, size_t last, simulation::FutureDag & future);
    model::HiCacheState & state_;
    std::map<size_t, Source> sources_;
    std::map<size_t, size_t> owners_;
    std::map<size_t, uint64_t> costs_;
    std::map<std::string, size_t> lane_tail_;
    std::map<size_t, Completion> completions_;
    std::set<std::pair<std::string, std::string>> submitted_;
    size_t completed_ = 0;
};

} // namespace markov::trace_graph::modules::hicache::runtime
