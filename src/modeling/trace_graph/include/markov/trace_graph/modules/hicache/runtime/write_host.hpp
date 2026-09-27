#pragma once
#include "markov/trace_graph/modules/hicache/patch/io_operation_ledger.hpp"

namespace markov::trace_graph::modules::hicache::runtime {

/** Source identities, not an aggregate host cost. Main-thread gaps and device
 * waits remain separate so a new branch cannot charge them twice. Boundaries
 * crossing a pure self leaf must be partitioned before this observation. */
struct HiCacheHostTemplate {
    patch::HiCacheTimingIntervalOwnership main;
    std::vector<size_t> worker_nodes;
    std::vector<size_t> device_wait_edges;
    std::optional<bool> write_back;
};

/** Structural ownership of a controller submission, independent of whether
 * the enclosing call can supply a reusable CPU/device cost template. */
[[nodiscard]] const patch::HiCacheSourceFactNode & observe_write_envelope(
    const patch::HiCacheSourceDagIndex & source, const std::string & pid, const std::string & tid, uint64_t begin, uint64_t end);

/** An explicitly nested tree.write_backup -> controller.start_writing pair
 * proves resource identity even when this base never performs a host load. */
[[nodiscard]] std::map<std::pair<std::string, std::string>, std::string> observe_write_controller_scopes(
    const patch::HiCacheSourceDagIndex & source);

[[nodiscard]] HiCacheHostTemplate observe_host_template(const patch::HiCacheSourceDagIndex & source, const std::string & pid, const std::string & tid,
                                                        uint64_t begin, uint64_t end);
[[nodiscard]] HiCacheHostTemplate observe_write_host_template(const patch::HiCacheSourceDagIndex & source, const patch::HiCacheIoOperationRecord & write);

/** A source eviction's ordered regions. WriteBackup is the complete outer
 * call, not start_writing. BlockingCheck still contains explicit device waits;
 * neither it nor Control is an aggregate service-time coefficient. */
struct HiCacheEvictionRegion {
    enum class Kind { Control, WriteBackup, BlockingCheck, ReleaseBackup, ReleaseRegular, SkipLocked };
    enum class ControlPhase { None, Setup, Selection, SelectionEnd, ReleaseStart, ReleaseNext, Finish, Empty };
    Kind kind;
    uint64_t begin, end;
    uint64_t released_tokens = 0;
    ControlPhase control_phase = ControlPhase::None;
    std::optional<Kind> preceding_kind;
};
[[nodiscard]] std::vector<HiCacheEvictionRegion> observe_eviction_regions(const patch::HiCacheSourceDagIndex & source,
                                                                          const patch::HiCacheSourceFactNode & eviction);

} // namespace markov::trace_graph::modules::hicache::runtime
