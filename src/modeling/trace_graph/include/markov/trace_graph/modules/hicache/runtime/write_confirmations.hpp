#pragma once
#include "markov/trace_graph/cost/operator_model.hpp"
#include "markov/trace_graph/modules/hicache/runtime/host_program.hpp"
#include "markov/trace_graph/modules/hicache/runtime/queue_confirmations.hpp"
#include "markov/trace_graph/modules/hicache/runtime/write_expansion.hpp"
#include "markov/trace_graph/structure/hicache/confirmation.hpp"
#include "markov/trace_graph/structure/hicache/resources.hpp"
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace markov::trace_graph::modules::hicache::runtime {
class HiCacheWriteCalls;

struct ConfirmationExpansion {
    HiCacheHostExpansion plan;
    std::vector<HostOperationBinding> operations;
    std::vector<size_t> acknowledgements;
};

/** Lower the declared confirmation program to caller, event waits and worker
 * FIFO tasks. Cost binding is separate and cannot change these dependencies. */
[[nodiscard]] ConfirmationExpansion generate_write_confirmation(const core::DagGraph & graph, size_t anchor, std::optional<size_t> worker,
                                                                std::span<const structure::hicache::WriteConfirmationStep> steps);

/** Independent nonblocking ACK host work. Source observations locate resources
 * and the old tail; target state supplies operations and their completion nodes. */
class HiCacheWriteConfirmations {
public:
    HiCacheWriteConfirmations(cost::OperatorCostProvider & provider, uint64_t page_size,
                              const structure::hicache::SchedulerComputeResources & compute_resources)
        : operator_provider_(provider),
          page_size_(page_size),
          compute_resources_(compute_resources) {}
    void prepare(core::DagGraph & graph, uint64_t begin, uint64_t end, const std::vector<core::TraceEvent> & replaced,
                 std::optional<uint64_t> idle_since_us = std::nullopt);
    void replace_source_tails(core::DagGraph & graph);
    size_t expand(const WriteConfirmationWork & work, model::HiCacheState & state, const HiCacheWriteCalls & writes, simulation::FutureDag & future);
    void advance(size_t node, uint64_t time_us, model::HiCacheState & state);

private:
    struct Tail {
        std::string pid, tid;
        uint64_t begin, end;
    };
    using Lane = std::pair<std::string, std::string>;
    std::vector<Tail> tails_;
    const core::DagGraph * graph_ = nullptr;
    cost::OperatorCostProvider & operator_provider_;
    uint64_t page_size_;
    const structure::hicache::SchedulerComputeResources & compute_resources_;
    std::map<Lane, size_t> worker_members_;
    std::map<size_t, HiCacheFact> acknowledgements_;
};
} // namespace markov::trace_graph::modules::hicache::runtime
