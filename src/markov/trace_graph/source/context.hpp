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
#include <utility>
#include <vector>

namespace markov::trace_graph::source {

struct WorkOrder {
    Index before;
    Index after;
};

enum class FrameCallKind { Compute, Preparation, Collective };

struct FrameCall {
    FrameCallKind kind;
    Index index;
    std::optional<Index> gap_before = std::nullopt;
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
    std::span<const CallBinding> preparation_bindings(Index frame) const { return preparation_bindings_[frame]; }
    const NativeSequenceBinding & preparation_native_calls(Index frame, Index preparation) const { return preparation_native_calls_[frame][preparation]; }
    std::span<const NativeWorkRegion> native_work() const { return native_work_; }
    void bind_native_sources(Index frame, Index preparation, std::span<planning::NativeCall> calls) const;
    std::span<const WorkOrder> frame_queue_order(Index frame) const { return frame_queue_order_[frame]; }
    std::span<const Index> frame_queue_issues(Index frame) const { return frame_queue_issues_[frame]; }
    std::span<const Index> frame_collectives(Index frame) const { return frame_collectives_[frame]; }
    std::span<const FrameCall> frame_calls(Index frame) const { return frame_calls_[frame]; }
    bool frame_cpu_order_supported(Index frame) const { return frame_cpu_order_supported_[frame]; }
    std::span<const ObservedRegion> cpu_gaps() const { return cpu_gaps_; }
    bool cpu_gap_has_unmodeled_work(Index interval) const { return cpu_gap_native_submissions_[interval]; }

private:
    void index_compute_queues();
    void index_cpu_intervals();
    void index_frame_calls();
    void index_native_sequences();
    void index_native_work();
    Index retain_native_work(planning::Work work, const char * scope, ObservedRegion region, bool stream_wait_generated = false,
                             NativeTiming timing = NativeTiming::Cpu, Time excluded_queue_wait_ns = 0, std::vector<ObservedRegion> excluded = {});

    Observations observations_;
    SourceGraph graph_;
    Time origin_ns_;
    std::vector<std::vector<Index>> frame_operations_;
    std::map<std::string, std::vector<Index>, std::less<>> request_frames_;
    std::vector<DeviceService> device_services_;
    std::vector<CpuInterval> cpu_work_intervals_;
    std::vector<std::optional<planning::CallResources>> compute_resources_;
    std::vector<std::vector<CpuInterval>> preparation_intervals_;
    std::vector<std::vector<CallBinding>> preparation_bindings_;
    std::vector<std::vector<NativeSequenceBinding>> preparation_native_calls_;
    std::vector<NativeWorkRegion> native_work_;
    std::map<std::pair<Index, planning::Work>, Index> control_sources_;
    std::vector<std::vector<WorkOrder>> frame_queue_order_;
    std::vector<std::vector<Index>> frame_queue_issues_;
    std::vector<std::vector<Index>> frame_collectives_;
    std::vector<std::vector<FrameCall>> frame_calls_;
    std::vector<bool> frame_cpu_order_supported_;
    std::vector<ObservedRegion> cpu_gaps_;
    std::vector<bool> cpu_gap_native_submissions_;
};

} // namespace markov::trace_graph::source
