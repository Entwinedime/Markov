#pragma once

#include "markov/trace_graph/simulation/execution.hpp"
#include "markov/trace_graph/source/build.hpp"
#include "markov/trace_graph/source/observations.hpp"
#include "markov/trace_graph/source/services.hpp"

#include <functional>
#include <map>
#include <memory>
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

struct SourceSequence {
    Index frame;
    bool result_processing;
    // Observations are finalized before indexing; their storage outlives these views.
    const ObservedCallSequence & observation;
    CpuInterval cpu;
    CallBinding resources;
    NativeSequenceBinding native;
};

// Own source observations and their graph once. Consumers borrow const views;
// target state and corrected cost views cannot mutate the original capture.
class SourceDetails;
class Regions;

class SourceContext {
public:
    explicit SourceContext(Observations && observations);
    SourceContext(const SourceContext &) = delete;
    SourceContext & operator=(const SourceContext &) = delete;
    ~SourceContext();
    SourceContext(SourceContext && other) noexcept;
    SourceContext & operator=(SourceContext && other) noexcept;

    const Observations & observations() const { return observations_; }
    const SourceGraph & graph() const;
    const Regions & regions() const;
    Time origin_ns() const { return origin_ns_; }
    Index client_resource() const { return client_resource_; }
    double detailed_preparation_sec() const { return detailed_preparation_sec_; }
    simulation::ExecutionResult replay() const;
    std::span<const Index> frame_operations(Index frame) const { return frame_operations_[frame]; }
    std::span<const Index> request_frames(std::string_view request) const;
    const BatchResult * frame_result(Index frame) const;
    const DeviceService & device_service(Index work) const;
    const CpuInterval & cpu_work_interval(Index work) const;
    const std::optional<planning::CallResources> & compute_resources(Index work) const;
    const SourceSequence & sequence(Index id) const;
    std::span<const Index> frame_sequences(Index frame) const;
    std::span<const Index> result_sequences(Index frame) const;
    std::span<const Index> lookup_sequences(Index frame) const;
    std::span<const NativeWorkRegion> native_work() const;
    void bind_native_sources(Index sequence, std::span<planning::NativeCall> calls, planning::CallResources resources) const;
    std::span<const WorkOrder> frame_queue_order(Index frame) const;
    std::span<const Index> frame_queue_issues(Index frame) const;
    std::span<const Index> frame_collectives(Index frame) const;
    std::span<const FrameCall> frame_calls(Index frame) const;
    bool frame_cpu_order_supported(Index frame) const;
    std::span<const ObservedRegion> cpu_gaps() const;
    bool cpu_gap_has_unmodeled_work(Index interval) const;

private:
    const SourceDetails & details() const;

    Observations observations_;
    Time origin_ns_;
    Index client_resource_;
    std::vector<std::vector<Index>> frame_operations_;
    std::map<std::string, std::vector<Index>, std::less<>> request_frames_;
    std::vector<std::optional<Index>> frame_results_;
    // The session's preparation thread creates details before sharing native views.
    mutable std::unique_ptr<SourceDetails> details_;
    mutable std::unique_ptr<Regions> regions_;
    mutable double detailed_preparation_sec_ = 0;
};

} // namespace markov::trace_graph::source
