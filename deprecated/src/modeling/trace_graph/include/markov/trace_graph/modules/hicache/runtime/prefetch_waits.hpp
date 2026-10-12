#pragma once
#include "markov/trace_graph/core/client_requests.hpp"
#include "markov/trace_graph/cost/operator_model.hpp"
#include "markov/trace_graph/modules/hicache/model/prefetch_wait_execution.hpp"
#include "markov/trace_graph/modules/hicache/model/replay.hpp"
#include "markov/trace_graph/modules/hicache/patch/prefetch_control_region.hpp"

namespace markov::trace_graph::modules::hicache::runtime {

/** Replaces source wait regions with target-driven loops over one serial HTTP
 * window. Uses independent active-branch costs, not source decisions or retry counts.
 * A corresponding unchanged source local return can retain its own cost.
 * Source policy parses source observations; target policy drives execution.
 * Bind after candidate boundaries, before global confirmations/CPU rewrites;
 * exclude replaced_intervals() from those global confirmations.
 */
class HiCachePrefetchWaits {
public:
    void bind(core::DagGraph & graph, model::HiCacheModelReplay & replay, std::string_view source_policy, const std::string & target_policy,
              const core::ClientRequestChain & chain, cost::OperatorCostProvider & costs);
    void start(simulation::FutureDag & future);
    void advance(size_t node, uint64_t absolute_time_us, simulation::FutureDag & future);
    [[nodiscard]] const std::vector<core::TraceEvent> & replaced_intervals() const { return replaced_; }

private:
    struct Wait {
        std::string request;
        std::map<int, HiCacheFact> facts;
        std::vector<patch::PrefetchControlRegion> regions;
        std::map<model::PrefetchSchedulerAction, std::map<int, uint64_t>> samples;
        std::unique_ptr<model::HiCachePrefetchWaitExecution> execution;
    };
    void scheduler_action(Wait & wait, const model::PrefetchSchedulerBoundary & boundary, uint64_t time);
    std::vector<std::unique_ptr<Wait>> waits_;
    std::vector<core::ClientRequestNodes> clients_;
    std::optional<std::string> active_http_;
    model::HiCacheState * state_ = nullptr;
    std::vector<core::TraceEvent> replaced_;
};

} // namespace markov::trace_graph::modules::hicache::runtime
