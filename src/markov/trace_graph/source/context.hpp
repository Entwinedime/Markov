#pragma once

#include "markov/trace_graph/simulation/execution.hpp"
#include "markov/trace_graph/source/build.hpp"
#include "markov/trace_graph/source/observations.hpp"
#include "markov/trace_graph/source/services.hpp"

#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace markov::trace_graph::source {

struct WorkOrder {
    Index before;
    Index after;
};

// Own source observations and their graph once. Consumers borrow const views;
// target state and corrected cost views cannot mutate the original capture.
class SourceContext {
public:
    explicit SourceContext(Observations && observations);
    SourceContext(const SourceContext &) = delete;
    SourceContext & operator=(const SourceContext &) = delete;
    SourceContext(SourceContext &&) = default;
    SourceContext & operator=(SourceContext &&) = default;

    const Observations & observations() const { return observations_; }
    const SourceGraph & graph() const { return graph_; }
    Time origin_ns() const { return origin_ns_; }
    simulation::ExecutionResult replay() const;
    std::span<const Index> frame_operations(Index frame) const { return frame_operations_[frame]; }
    std::span<const Index> request_frames(std::string_view request) const;
    const DeviceService & device_service(Index work) const { return device_services_[work]; }
    const CpuInterval & cpu_work_interval(Index work) const { return cpu_work_intervals_[work]; }
    const std::optional<planning::CallResources> & compute_resources(Index work) const { return compute_resources_[work]; }
    std::span<const CpuInterval> preparation_intervals(Index frame) const { return preparation_intervals_[frame]; }
    std::span<const std::optional<planning::CallResources>> preparation_resources(Index frame) const { return preparation_resources_[frame]; }
    std::span<const WorkOrder> frame_queue_order(Index frame) const { return frame_queue_order_[frame]; }
    std::span<const Index> frame_queue_issues(Index frame) const { return frame_queue_issues_[frame]; }
    std::span<const Index> frame_collectives(Index frame) const { return frame_collectives_[frame]; }

private:
    void index_compute_queues();
    void index_cpu_intervals();

    Observations observations_;
    SourceGraph graph_;
    Time origin_ns_;
    std::vector<std::vector<Index>> frame_operations_;
    std::map<std::string, std::vector<Index>, std::less<>> request_frames_;
    std::vector<DeviceService> device_services_;
    std::vector<CpuInterval> cpu_work_intervals_;
    std::vector<std::optional<planning::CallResources>> compute_resources_;
    std::vector<std::vector<CpuInterval>> preparation_intervals_;
    std::vector<std::vector<std::optional<planning::CallResources>>> preparation_resources_;
    std::vector<std::vector<WorkOrder>> frame_queue_order_;
    std::vector<std::vector<Index>> frame_queue_issues_;
    std::vector<std::vector<Index>> frame_collectives_;
};

} // namespace markov::trace_graph::source
