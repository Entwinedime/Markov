#pragma once
#include "markov/trace_graph/modules/hicache/runtime/queue_confirmations.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_expansion.hpp"

namespace markov::trace_graph::modules::hicache::runtime {
class HiCacheWriteCalls;

/** Empty or repeated single-batch ACK work. Target events determine count and
 * FIFO order; independent costs need not contain that aggregate count. */
size_t expand_write_confirmation_batches(const HiCacheHostExpansion & empty, const HiCacheHostExpansion & single, std::span<const size_t> completions,
                                         std::optional<size_t> anchor, simulation::FutureDag & future);

/** Independent nonblocking ACK host work. Source observations locate resources
 * and the old tail; target state supplies operations and their completion nodes. */
class HiCacheWriteConfirmations {
public:
    void prepare(core::DagGraph & graph, const std::string & calibration, uint64_t begin, uint64_t end, const std::vector<core::TraceEvent> & replaced,
                 std::optional<uint64_t> idle_since_us = std::nullopt);
    void replace_source_tails(core::DagGraph & graph);
    void rebind_workers(const core::DagGraph & graph, const std::map<size_t, size_t> & members);
    size_t expand(const WriteConfirmationWork & work, model::HiCacheState & state, const HiCacheWriteCalls & writes, simulation::FutureDag & future) const;

private:
    struct Tail {
        std::string pid, tid;
        uint64_t begin, end;
    };
    using Lane = std::pair<std::string, std::string>;
    std::vector<Tail> tails_;
    std::map<Lane, std::map<uint64_t, HiCacheHostExpansion>> templates_;
};
} // namespace markov::trace_graph::modules::hicache::runtime
