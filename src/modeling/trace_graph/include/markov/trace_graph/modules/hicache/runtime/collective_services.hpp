#pragma once

#include "markov/trace_graph/cost/operator_model.hpp"
#include "markov/trace_graph/source_dag/device_collectives.hpp"
#include "markov/trace_graph/structure/hicache/phase.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <vector>

namespace markov::trace_graph::modules::hicache::runtime {

/** Replace attributed communication service and its queued CPU tasks.
 * Each rank has an input-ready gate and a service node constrained by every
 * participant's input. Source service durations are never copied. Existing
 * result consumers keep their dependencies through atomic edge redirection.
 * The initial lowering handles one communication device node per call;
 * multi-launch/stream protocols require a separate lowering, not aggregation.
 * Event tasks remain separate from reduction work. Caller preparation is
 * generated per enqueue, retaining overlap with prior submitted tasks. Device
 * event services use independent measurements, while their dependency
 * correspondence is still source-derived. Dispatch migrates the base's corrected
 * scheduler residual, not its queue wait. CPU service uses independent costs.
 */
class HiCacheCollectiveServices {
public:
    void bind(core::DagGraph & graph, const core::DagGraph & original, std::span<const source_dag::DeviceCollectiveCall> calls, uint64_t begin_us,
              uint64_t end_us);
    void set_work(const structure::hicache::DeviceCollectivePlan & work);
    /** Null for unrelated nodes. A service reached without admitted target
     * work is an ordering error, not an implicit zero or source fallback. */
    [[nodiscard]] std::optional<structure::OperationWork> demand(const core::DagGraph & graph, size_t node) const;
    /** Bind service/residual scopes for a generated queued worker segment.
     * Null for unrelated nodes; no topology or queue membership is changed. */
    [[nodiscard]] std::optional<uint64_t> worker_cost(core::DagGraph & graph, size_t node, cost::OperatorCostProvider & provider) const;
    [[nodiscard]] std::optional<uint64_t> caller_cost(core::DagGraph & graph, size_t node, cost::OperatorCostProvider & provider) const;
    [[nodiscard]] bool owns(size_t node) const { return owned_.contains(node); }
    [[nodiscard]] size_t prepared() const { return service_observations_.size(); }
    [[nodiscard]] size_t prepared_events() const { return event_services_.size(); }
    [[nodiscard]] size_t prepared_workers() const { return worker_segments_.size(); }
    [[nodiscard]] size_t prepared_callers() const { return caller_segments_.size(); }

private:
    void bind_callers(core::DagGraph & graph, const core::DagGraph & original, std::span<const source_dag::DeviceCollectiveCall> calls, uint64_t begin_us,
                      uint64_t end_us);

    std::set<size_t> owned_;
    std::map<size_t, size_t> service_observations_;
    struct EventService {
        size_t observation;
        structure::hicache::CollectiveTaskKind kind;
    };
    std::map<size_t, EventService> event_services_;
    std::map<size_t, size_t> arrival_nodes_;
    std::map<size_t, std::vector<size_t>> peers_;
    std::map<size_t, structure::OperationWork> work_;
    struct WorkerSegment {
        enum class Part { Launch, Return, Whole };
        size_t observation;
        structure::hicache::CollectiveTaskKind kind;
        Part part;
    };
    std::map<size_t, WorkerSegment> worker_segments_;
    struct CallerSegment {
        size_t observation, step;
        structure::hicache::CollectiveTaskKind transfer;
    };
    std::map<size_t, CallerSegment> caller_segments_;
};

} // namespace markov::trace_graph::modules::hicache::runtime
