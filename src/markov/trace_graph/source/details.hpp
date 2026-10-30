#pragma once

#include "markov/trace_graph/source/context.hpp"

namespace markov::trace_graph::source {

// Fine source materialization is optional. Normal region planning reads the
// immutable semantic observations without allocating this graph or its indexes.
class SourceDetails {
public:
    explicit SourceDetails(const SourceContext & source);

private:
    friend class SourceContext;
    void bind_native_sources(Index sequence, std::span<planning::NativeCall> calls, planning::CallResources resources) const;
    void index_compute_queues();
    void index_cpu_intervals();
    void index_frame_calls();
    void index_native_sequences();
    void index_native_work();
    Index retain_native_work(planning::Work work, const char * scope, ObservedRegion region, HostWait generated_wait = HostWait::None,
                             NativeTiming timing = NativeTiming::Cpu, Time excluded_queue_wait_ns = 0, std::vector<ObservedRegion> excluded = {});


    const Observations & observations_;
    const SourceContext & source_;
    SourceGraph graph_;
    std::vector<DeviceService> device_services_;
    std::vector<CpuInterval> cpu_work_intervals_;
    std::vector<std::optional<planning::CallResources>> compute_resources_;
    std::vector<SourceSequence> sequences_;
    std::vector<std::vector<Index>> frame_sequences_;
    std::vector<std::vector<Index>> result_sequences_;
    std::vector<std::vector<Index>> lookup_sequences_;
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
