#pragma once
#include "markov/trace_graph/adapters/frameworks/sglang/hicache_observations.hpp"
#include "markov/trace_graph/modules/hicache/patch/io_operation_ledger.hpp"
#include "markov/trace_graph/source_dag/index.hpp"

namespace markov::trace_graph::modules::hicache::runtime {

/** Source identities, not an aggregate host cost. Main-thread gaps and device
 * waits remain separate so a new branch cannot charge them twice. Boundaries
 * crossing a pure self leaf must be partitioned before this observation. */
struct HiCacheHostTemplate {
    source_dag::TimingIntervalOwnership main;
    std::vector<size_t> worker_nodes;
    std::vector<size_t> device_wait_edges;
    std::optional<bool> write_back;
};

/** Structural ownership of a controller submission, independent of whether
 * the enclosing call can supply a reusable CPU/device cost template. */
[[nodiscard]] const adapters::sglang::HiCacheFactObservation &
    observe_write_envelope(const adapters::sglang::HiCacheSourceIndex & source, const std::string & pid, const std::string & tid, uint64_t begin, uint64_t end);

/** Query facts directly name the tree's controller. An explicitly nested
 * tree.write_backup -> controller.start_writing pair supplies the same
 * association when the source records only call identities. */
[[nodiscard]] std::map<std::pair<std::string, std::string>, std::string> observe_controller_scopes(const adapters::sglang::HiCacheSourceIndex & source);

[[nodiscard]] HiCacheHostTemplate observe_host_template(const source_dag::SourceDagIndex & source, const std::string & pid, const std::string & tid,
                                                        uint64_t begin, uint64_t end);
[[nodiscard]] HiCacheHostTemplate observe_write_host_template(const adapters::sglang::HiCacheSourceIndex & source,
                                                              const patch::HiCacheIoOperationRecord & write);

/** A source eviction's ordered regions. WriteBackup is the complete outer
 * call, not start_writing. BlockingCheck still contains explicit device waits;
 * neither it nor Control is an aggregate service-time coefficient. */
struct HiCacheEvictionRegion {
    enum class Kind { Control, WriteBackup, BlockingCheck, ReleaseBackup, ReleaseRegular, SkipLocked, RejectedBackup };
    enum class ControlPhase { None, Setup, Selection, SelectionEnd, ReleaseStart, ReleaseNext, Finish, Empty };
    Kind kind;
    uint64_t begin, end;
    uint64_t released_tokens = 0;
    ControlPhase control_phase = ControlPhase::None;
    std::optional<Kind> preceding_kind;
};
[[nodiscard]] std::vector<HiCacheEvictionRegion> observe_eviction_regions(const adapters::sglang::HiCacheSourceIndex & source,
                                                                          const adapters::sglang::HiCacheFactObservation & eviction);

} // namespace markov::trace_graph::modules::hicache::runtime
