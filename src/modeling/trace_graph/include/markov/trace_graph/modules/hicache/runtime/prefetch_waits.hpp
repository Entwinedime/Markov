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
struct PrefetchWaitReturn {
    std::string request_id;
    int rank;
    uint64_t timestamp_us;
    size_t checks, visible_pages;
    bool stopped;
    // Snapshot at foreground return; absent timestamps are unavailable or pending.
    std::optional<uint64_t> query_return_us, io_start_us, first_page_us, last_page_us;
    std::optional<uint64_t> stop_us, visible_us, worker_return_us;
};
struct PrefetchSchedulerReturn {
    std::string request_id;
    int rank;
    uint64_t timestamp_us;
    model::PrefetchSchedulerAction action;
    uint64_t batches = 0;
};
struct PrefetchWaitObserver {
    std::function<void(const PrefetchWaitReturn &)> returned;
    std::function<void(const PrefetchSchedulerReturn &)> scheduler;
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
              const core::ClientRequestChain & chain, PrefetchWaitObserver observer = {}, const std::string & calibration_path = {},
              const std::vector<std::string> & cpu_calibrations = {});
    void start(simulation::FutureDag & future);
    void advance(size_t node, uint64_t absolute_time_us, simulation::FutureDag & future);
    [[nodiscard]] const std::vector<core::TraceEvent> & replaced_intervals() const { return replaced_; }
    [[nodiscard]] const PrefetchWaitTemplate & source_template() const { return template_; }
    [[nodiscard]] size_t request_count() const { return waits_.size(); }

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
    PrefetchWaitObserver observer_;
};

} // namespace markov::trace_graph::modules::hicache::runtime
