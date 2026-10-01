#pragma once
#include "markov/trace_graph/core/client_requests.hpp"
#include "markov/trace_graph/modules/hicache/model/prefetch_wait_execution.hpp"
#include "markov/trace_graph/modules/hicache/model/replay.hpp"
#include "markov/trace_graph/modules/hicache/patch/prefetch_control_region.hpp"

namespace markov::trace_graph::modules::hicache::runtime {

struct PrefetchWaitTemplate {
    std::string request_id;
    std::map<int, model::PrefetchCheckCpuTiming> cpu;
    std::string calibration_manifest;
    std::map<int, std::string> local_return_calibration_manifests;
};
/** Replaces source wait regions with target-driven loops over one serial HTTP
 * window. Uses same-base branch work, not source decisions or retry counts.
 * Source policy parses source observations; target policy drives execution.
 * Bind after candidate boundaries, before global confirmations/CPU rewrites;
 * exclude replaced_intervals() from those global confirmations.
 */
class HiCachePrefetchWaits {
public:
    void bind(core::DagGraph & graph, model::HiCacheModelReplay & replay, std::string_view source_policy, const std::string & target_policy,
              const core::ClientRequestChain & chain, const std::string & calibration_path = {}, const std::vector<std::string> & cpu_calibrations = {});
    void start(simulation::FutureDag & future);
    void advance(size_t node, uint64_t absolute_time_us, simulation::FutureDag & future);
    [[nodiscard]] const std::vector<core::TraceEvent> & replaced_intervals() const { return replaced_; }
    [[nodiscard]] const PrefetchWaitTemplate & source_template() const { return template_; }

private:
    struct Wait {
        std::string request;
        std::map<int, HiCacheFact> facts;
        std::vector<patch::PrefetchControlRegion> regions;
        std::map<size_t, std::map<int, uint64_t>> samples;
        std::unique_ptr<model::HiCachePrefetchWaitExecution> execution;
    };
    void scheduler_action(Wait & wait, model::PrefetchSchedulerAction action, const model::PrefetchSchedulerBoundary & boundary, uint64_t time);
    std::vector<std::unique_ptr<Wait>> waits_;
    std::vector<core::ClientRequestNodes> clients_;
    std::optional<std::string> active_http_;
    model::HiCacheState * state_ = nullptr;
    std::vector<core::TraceEvent> replaced_;
    PrefetchWaitTemplate template_;
};

} // namespace markov::trace_graph::modules::hicache::runtime
